#include "PocketTTSImpl.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

// NNAPI is intentionally not used for PocketTTS: the stateful KV-cache ops in
// flow_lm_main_int8.onnx are not supported by NNAPI's model builder, causing
// ANEURALNETWORKS_BAD_DATA at compile time. The INT8 models run correctly on CPU.

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr float  kEosThreshold    = -4.0f; // Python DEFAULT_EOS_THRESHOLD
static constexpr int    kMaxFrames       = 500;    // absolute cap on AR loop length
static constexpr int    kMaxChunkTokens  = 50;     // Python MAX_TOKEN_PER_CHUNK

// Python TTSModel._estimate_max_gen_len: gen budget scales with the text length
// instead of always running to the absolute cap when EOS is missed.
static constexpr double kTokensPerSecondEstimate = 3.0;  // _TOKENS_PER_SECOND_ESTIMATE
static constexpr double kGenSecondsPadding       = 2.0;  // _GEN_SECONDS_PADDING
static constexpr double kFrameRate               = 12.5; // mimi frame_rate

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<std::string> fetchNames(Ort::Session& sess, bool inputs) {
    Ort::AllocatorWithDefaultOptions alloc;
    const size_t n = inputs ? sess.GetInputCount() : sess.GetOutputCount();
    std::vector<std::string> names(n);
    for (size_t i = 0; i < n; ++i) {
        auto ptr = inputs ? sess.GetInputNameAllocated(i,  alloc)
                          : sess.GetOutputNameAllocated(i, alloc);
        names[i] = ptr.get();
    }
    return names;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PocketTTSImpl::PocketTTSImpl(const std::string& modelDir, const std::string& voiceName)
    : modelDir_(modelDir)
    , rng_(std::random_device{}())
{
    params_.voiceName = voiceName;
}

// ---------------------------------------------------------------------------
// initialize()
// ---------------------------------------------------------------------------

bool PocketTTSImpl::initialize() {
    if (initialized_) return true;

    sessionOpts_.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);
    sessionOpts_.SetIntraOpNumThreads(4);

    if (!loadTokenizer()) return false;
    if (!loadVoices())    return false;
    if (!loadModels())    return false;

    // Pre-allocate flow LM state buffers
    for (auto& buf : kvBufs_)   buf.assign(kKVElems, 0.0f);
    for (auto& buf : stepBufs_) buf.assign(1, 0LL);
    emptyFloatBuf_.assign(1, 0.0f); // data() must be non-null (shape passed as [0])

    introspectMimiDecStates();

    initialized_ = true;
    spdlog::info("[PocketTTS] Initialized. Voices: {}, SampleRate: 24000 Hz", voices_.size());
    return true;
}

// ---------------------------------------------------------------------------
// loadTokenizer
// ---------------------------------------------------------------------------

bool PocketTTSImpl::loadTokenizer() {
    const std::string path = modelDir_ + "/tokenizer.model";
    if (!std::filesystem::exists(path)) {
        spdlog::error("[PocketTTS] tokenizer.model not found: {}", path);
        return false;
    }
    tokenizer_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
    const auto status = tokenizer_->Load(path);
    if (!status.ok()) {
        spdlog::error("[PocketTTS] SentencePiece load failed: {}", status.ToString());
        return false;
    }
    spdlog::info("[PocketTTS] Tokenizer loaded (vocab size: {})", tokenizer_->GetPieceSize());
    return true;
}

// ---------------------------------------------------------------------------
// loadVoices
// ---------------------------------------------------------------------------

bool PocketTTSImpl::loadVoices() {
    const std::string path = modelDir_ + "/voices.bin";
    if (!std::filesystem::exists(path)) {
        spdlog::error("[PocketTTS] voices.bin not found: {}", path);
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        spdlog::error("[PocketTTS] Cannot open voices.bin");
        return false;
    }

    // Slurp entire file for safe bounds-checked reads
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f), {});
    size_t offset = 0;

    auto need = [&](size_t n) -> bool { return offset + n <= buf.size(); };

    auto readBytes = [&](void* dst, size_t n) -> bool {
        if (!need(n)) return false;
        std::memcpy(dst, buf.data() + offset, n);
        offset += n;
        return true;
    };

    // 5-byte ASCII magic "PTVB1"
    char magic[5] = {};
    if (!readBytes(magic, 5) || std::string(magic, 5) != "PTVB1") {
        spdlog::error("[PocketTTS] voices.bin: invalid or missing PTVB1 magic");
        return false;
    }

    uint32_t numVoices = 0;
    if (!readBytes(&numVoices, 4) || numVoices == 0 || numVoices > 65536) {
        spdlog::error("[PocketTTS] voices.bin: invalid voice count {}", numVoices);
        return false;
    }

    voices_.resize(numVoices);
    for (uint32_t vi = 0; vi < numVoices; ++vi) {
        // Variable-length name
        uint16_t nameLen = 0;
        if (!readBytes(&nameLen, 2)) {
            spdlog::error("[PocketTTS] voices.bin: truncated at voice {} name length", vi);
            return false;
        }
        voices_[vi].name.resize(nameLen);
        if (!readBytes(voices_[vi].name.data(), nameLen)) {
            spdlog::error("[PocketTTS] voices.bin: truncated reading name of voice {}", vi);
            return false;
        }

        uint16_t tensorCount = 0;
        if (!readBytes(&tensorCount, 2)) {
            spdlog::error("[PocketTTS] voices.bin: truncated reading tensor count for '{}'",
                          voices_[vi].name);
            return false;
        }

        for (uint16_t ti = 0; ti < tensorCount; ++ti) {
            // Key (full path, e.g. "transformer.layers.0.self_attn/cache")
            uint16_t keyLen = 0;
            if (!readBytes(&keyLen, 2)) {
                spdlog::error("[PocketTTS] voices.bin: truncated at key length (voice '{}', tensor {})",
                              voices_[vi].name, ti);
                return false;
            }
            std::string key(keyLen, '\0');
            if (!readBytes(key.data(), keyLen)) {
                spdlog::error("[PocketTTS] voices.bin: truncated reading key (voice '{}', tensor {})",
                              voices_[vi].name, ti);
                return false;
            }

            TensorRecord rec;
            uint8_t rank = 0;
            if (!readBytes(&rec.dtype, 1) || !readBytes(&rank, 1)) {
                spdlog::error("[PocketTTS] voices.bin: truncated at dtype/rank for '{}/{}'",
                              voices_[vi].name, key);
                return false;
            }

            rec.shape.resize(rank);
            for (uint8_t d = 0; d < rank; ++d) {
                uint32_t dim = 0;
                if (!readBytes(&dim, 4)) {
                    spdlog::error("[PocketTTS] voices.bin: truncated reading shape for '{}/{}'",
                                  voices_[vi].name, key);
                    return false;
                }
                rec.shape[d] = dim;
            }

            uint32_t byteLen = 0;
            if (!readBytes(&byteLen, 4)) {
                spdlog::error("[PocketTTS] voices.bin: truncated at byte length for '{}/{}'",
                              voices_[vi].name, key);
                return false;
            }
            rec.bytes.resize(byteLen);
            if (byteLen > 0 && !readBytes(rec.bytes.data(), byteLen)) {
                spdlog::error("[PocketTTS] voices.bin: truncated reading tensor data for '{}/{}'",
                              voices_[vi].name, key);
                return false;
            }

            voices_[vi].tensors[key] = std::move(rec);
        }

        spdlog::debug("[PocketTTS] Voice '{}': {} tensor(s)", voices_[vi].name, tensorCount);
    }

    spdlog::info("[PocketTTS] Loaded {} voice(s) from voices.bin", voices_.size());
    return true;
}

// ---------------------------------------------------------------------------
// loadModels
// ---------------------------------------------------------------------------

bool PocketTTSImpl::loadModels() {
    struct ModelSpec { std::string filename; std::unique_ptr<Ort::Session>* session; };
    std::vector<ModelSpec> specs = {
        { "text_conditioner_int8.onnx", &textCondSession_  },
        { "flow_lm_main_int8.onnx",  &flowLmMainSession_  },
        { "flow_lm_flow_int8.onnx",  &flowLmFlowSession_  },
        { "mimi_decoder_int8.onnx",  &mimiDecSession_     },
    };

    for (auto& spec : specs) {
        const std::string fullPath = modelDir_ + "/" + spec.filename;
        if (!std::filesystem::exists(fullPath)) {
            spdlog::error("[PocketTTS] Model not found: {}", fullPath);
            return false;
        }
        try {
#ifdef _WIN32
            std::wstring wp(fullPath.begin(), fullPath.end());
            *spec.session = std::make_unique<Ort::Session>(env_, wp.c_str(), sessionOpts_);
#else
            *spec.session = std::make_unique<Ort::Session>(env_, fullPath.c_str(), sessionOpts_);
#endif
        } catch (const Ort::Exception& e) {
            spdlog::error("[PocketTTS] Failed to load {}: {}", spec.filename, e.what());
            return false;
        }
        spdlog::info("[PocketTTS] Loaded {}", spec.filename);
    }

    // Cache input/output names for all sessions
    textCondInNames_    = fetchNames(*textCondSession_,   true);
    textCondOutNames_   = fetchNames(*textCondSession_,   false);
    flowLmMainInNames_  = fetchNames(*flowLmMainSession_, true);
    flowLmMainOutNames_ = fetchNames(*flowLmMainSession_, false);
    flowLmFlowInNames_  = fetchNames(*flowLmFlowSession_, true);
    flowLmFlowOutNames_ = fetchNames(*flowLmFlowSession_, false);
    mimiDecInNames_     = fetchNames(*mimiDecSession_,    true);
    mimiDecOutNames_    = fetchNames(*mimiDecSession_,    false);

    return true;
}

// ---------------------------------------------------------------------------
// introspectMimiDecStates
// ---------------------------------------------------------------------------

void PocketTTSImpl::introspectMimiDecStates() {
    // The first input is "latent"; the remaining (up to kNumMimiDecStates) are states.
    // We determine each state's shape and element type from the model metadata.
    mimiDecStateBufs_.clear();
    mimiDecStateBoolBufs_.clear();
    mimiDecStateInt64Bufs_.clear();
    mimiDecStateShapes_.clear();
    mimiDecStateType_.clear();

    const size_t totalIn = mimiDecInNames_.size();
    // First input is always "latent" - skip it; the rest are states.
    const size_t numStates = totalIn > 0 ? totalIn - 1 : 0;
    mimiDecStateBufs_.resize(numStates);
    mimiDecStateBoolBufs_.resize(numStates);
    mimiDecStateInt64Bufs_.resize(numStates);
    mimiDecStateShapes_.resize(numStates);
    mimiDecStateType_.resize(numStates, MimiStateType::FLOAT);

    for (size_t s = 0; s < numStates; ++s) {
        try {
            auto typeInfo = mimiDecSession_->GetInputTypeInfo(1 + s);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            auto shape = tensorInfo.GetShape();
            const ONNXTensorElementDataType elemType = tensorInfo.GetElementType();
            for (auto& d : shape) { if (d < 0) d = 1; }
            mimiDecStateShapes_[s].assign(shape.begin(), shape.end());
            size_t elems = 1;
            for (auto d : shape) elems *= static_cast<size_t>(d);

            if (elemType == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) {
                mimiDecStateType_[s] = MimiStateType::BOOL;
                mimiDecStateBoolBufs_[s].assign(elems, false);
            } else if (elemType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
                mimiDecStateType_[s] = MimiStateType::INT64;
                mimiDecStateInt64Bufs_[s].assign(elems, 0LL);
            } else {
                mimiDecStateType_[s] = MimiStateType::FLOAT;
                mimiDecStateBufs_[s].assign(elems, 0.0f);
            }
            spdlog::trace("[PocketTTS] MimiDec state {} type={} shape: [{}]", s,
                          (mimiDecStateType_[s] == MimiStateType::BOOL ? "bool" :
                           mimiDecStateType_[s] == MimiStateType::INT64 ? "int64" : "float"),
                          [&]{ std::string r; for (auto d : shape) r += std::to_string(d) + ","; return r; }());
        } catch (const Ort::Exception& e) {
            spdlog::warn("[PocketTTS] Could not introspect mimi_decoder state {}: {}", s, e.what());
            mimiDecStateShapes_[s] = {1};
            mimiDecStateBufs_[s]   = {0.0f};
            mimiDecStateType_[s]   = MimiStateType::FLOAT;
        }
    }
    spdlog::info("[PocketTTS] MIMI decoder: {} state buffer(s) pre-allocated", numStates);
}

// ---------------------------------------------------------------------------
// Parameter access
// ---------------------------------------------------------------------------

void PocketTTSImpl::setParams(const Params& p) {
    std::lock_guard<std::mutex> lk(paramsMutex_);
    params_ = p;
}

PocketTTSImpl::Params PocketTTSImpl::getParams() const {
    std::lock_guard<std::mutex> lk(paramsMutex_);
    return params_;
}

std::vector<std::string> PocketTTSImpl::getVoiceNames() const {
    std::vector<std::string> names;
    names.reserve(voices_.size());
    for (const auto& v : voices_) names.push_back(v.name);
    return names;
}

// ---------------------------------------------------------------------------
// tokenize / detokenize / chunkBySentences
// ---------------------------------------------------------------------------

std::vector<int64_t> PocketTTSImpl::tokenize(const std::string& text) const {
    std::vector<int> ids;
    tokenizer_->Encode(text, &ids);
    return std::vector<int64_t>(ids.begin(), ids.end());
}

std::string PocketTTSImpl::detokenize(const std::vector<int64_t>& ids) const {
    std::vector<int> int_ids;
    int_ids.reserve(ids.size());
    for (auto id : ids) int_ids.push_back(static_cast<int>(id));
    std::string result;
    tokenizer_->Decode(int_ids, &result);
    return result;
}

// JS equivalent: splitIntoBestSentences
std::vector<std::string> PocketTTSImpl::chunkBySentences(
    const std::string& text, int maxTokens) const
{
    if (text.empty()) return {};

    // --- Step 1: split at .!? sentence boundaries ---
    std::vector<std::string> sentences;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ) {
        const char c = text[i];
        if (c == '.' || c == '!' || c == '?') {
            // Consume consecutive terminators (e.g. "..." or "!?")
            size_t end = i;
            while (end + 1 < text.size() &&
                   (text[end + 1] == '.' || text[end + 1] == '!' || text[end + 1] == '?'))
                ++end;
            // Extract and trim the sentence
            std::string s = text.substr(start, end + 1 - start);
            size_t l = 0;
            while (l < s.size() && std::isspace((unsigned char)s[l]))    ++l;
            size_t r = s.size();
            while (r > l  && std::isspace((unsigned char)s[r - 1]))      --r;
            if (r > l) sentences.push_back(s.substr(l, r - l));
            // Skip whitespace after terminators
            start = end + 1;
            while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
            i = start;
        } else {
            ++i;
        }
    }
    // Trailing fragment with no terminator
    if (start < text.size()) {
        std::string s = text.substr(start);
        size_t l = 0;
        while (l < s.size() && std::isspace((unsigned char)s[l]))    ++l;
        size_t r = s.size();
        while (r > l  && std::isspace((unsigned char)s[r - 1]))      --r;
        if (r > l) sentences.push_back(s.substr(l, r - l));
    }
    if (sentences.empty()) return {text}; // fallback: treat whole text as one chunk

    // --- Step 1.5: refine oversized sentences ---
    // Python split_into_best_sentences sub-splits sentences that exceed the
    // token budget at clause boundaries (, ; :) to prevent skipped words.
    // If a clause is still too long we force-split at word boundaries (never
    // mid-word, which a raw token-slice split could do).
    auto splitOnClauses = [](const std::string& s) {
        std::vector<std::string> parts;
        size_t begin = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == ',' || c == ';' || c == ':') {
                while (i + 1 < s.size() &&
                       (s[i + 1] == ',' || s[i + 1] == ';' || s[i + 1] == ':'))
                    ++i;
                std::string part = s.substr(begin, i + 1 - begin);
                size_t l = part.find_first_not_of(" \t");
                if (l != std::string::npos) parts.push_back(part.substr(l));
                begin = i + 1;
            }
        }
        if (begin < s.size()) {
            std::string part = s.substr(begin);
            size_t l = part.find_first_not_of(" \t");
            if (l != std::string::npos) parts.push_back(part.substr(l));
        }
        return parts;
    };

    auto splitByWords = [this, maxTokens](const std::string& s,
                                          std::vector<std::string>& out) {
        std::istringstream iss(s);
        std::string word, acc;
        while (iss >> word) {
            const std::string candidate = acc.empty() ? word : acc + " " + word;
            if (!acc.empty() &&
                static_cast<int>(tokenize(candidate).size()) > maxTokens) {
                out.push_back(acc);
                acc = word;
            } else {
                acc = candidate;
            }
        }
        if (!acc.empty()) out.push_back(acc);
    };

    std::vector<std::string> segments;
    for (const auto& sentence : sentences) {
        if (static_cast<int>(tokenize(sentence).size()) <= maxTokens) {
            segments.push_back(sentence);
            continue;
        }
        const auto clauses = splitOnClauses(sentence);
        if (clauses.size() > 1) {
            for (const auto& clause : clauses) {
                if (static_cast<int>(tokenize(clause).size()) <= maxTokens)
                    segments.push_back(clause);
                else
                    splitByWords(clause, segments);
            }
        } else {
            splitByWords(sentence, segments);
        }
    }

    // --- Step 2: greedily merge segments into chunks <= maxTokens ---
    std::vector<std::string> chunks;
    std::string current;
    for (const auto& sentence : segments) {
        if (current.empty()) { current = sentence; continue; }

        const std::string combined = current + " " + sentence;
        if (static_cast<int>(tokenize(combined).size()) <= maxTokens) {
            current = combined;
        } else {
            chunks.push_back(current);
            current = sentence;
        }
    }
    if (!current.empty()) chunks.push_back(current);
    return chunks;
}

// ---------------------------------------------------------------------------
// Voice-conditioning cache  (JS: buildVoiceConditionedState / cloneFlowState)
// ---------------------------------------------------------------------------

void PocketTTSImpl::refreshVoiceState(const VoiceEntry* voice) {
    if (voiceCondState_.name == voice->name) {
        // Cache hit: restore pre-loaded state into active buffers
        kvBufs_   = voiceCondState_.kvBufs;
        stepBufs_ = voiceCondState_.stepBufs;
        return;
    }

    // Cache miss: populate state buffers directly from pre-computed voice tensors.
    // voices.bin (PTVB1) stores the flow LM KV states that result from voice
    // conditioning, so we just copy them - no runFlowLMMain() call needed.

    // KV buffers: fill with NaN (manifest fill="nan" for unused cache slots).
    // Step buffers: fill with 0.
    for (auto& buf : kvBufs_)   std::fill(buf.begin(), buf.end(), std::numeric_limits<float>::quiet_NaN());
    for (auto& buf : stepBufs_) std::fill(buf.begin(), buf.end(), 0LL);

    for (int layer = 0; layer < 6; ++layer) {
        const std::string prefix = "transformer.layers." + std::to_string(layer) + ".self_attn/";

        // KV cache: source shape is [kv=2, b=1, srcSeq, heads=16, dim=64].
        // Target buffer is [kv=2, b=1, tgtSeq=1000, heads=16, dim=64].
        // We must do a strided copy so that the k=0 and k=1 blocks land at the
        // correct offsets in the larger target - a flat memcpy is wrong because
        // the k=1 source block needs to jump to offset kv*tgtSeq*headDim.
        auto cacheIt = voice->tensors.find(prefix + "cache");
        if (cacheIt != voice->tensors.end()) {
            const auto& rec = cacheIt->second;
            if (rec.shape.size() == 5) {
                const uint32_t srcKV   = rec.shape[0]; // 2
                const uint32_t srcSeq  = rec.shape[2]; // e.g. 126
                const uint32_t headDim = rec.shape[3] * rec.shape[4]; // 16×64 = 1024
                const uint32_t tgtSeq  = static_cast<uint32_t>(kKVShape[2]); // 1000

                const float* src = reinterpret_cast<const float*>(rec.bytes.data());
                for (uint32_t k = 0; k < srcKV; ++k) {
                    for (uint32_t s = 0; s < srcSeq && s < tgtSeq; ++s) {
                        const size_t srcOff = (k * srcSeq + s) * headDim;
                        const size_t dstOff = (k * tgtSeq + s) * headDim;
                        std::memcpy(&kvBufs_[layer][dstOff], src + srcOff,
                                    headDim * sizeof(float));
                    }
                }
            }
        }

        // Offset key in voices.bin is "offset" (JS derives "step" from it).
        auto offsetIt = voice->tensors.find(prefix + "offset");
        if (offsetIt != voice->tensors.end()) {
            const auto& rec = offsetIt->second;
            if (rec.bytes.size() >= sizeof(int64_t))
                stepBufs_[layer][0] = *reinterpret_cast<const int64_t*>(rec.bytes.data());
        }
    }

    voiceCondState_.name     = voice->name;
    voiceCondState_.kvBufs   = kvBufs_;
    voiceCondState_.stepBufs = stepBufs_;
    spdlog::debug("[PocketTTS] Voice state loaded from voices.bin for '{}'", voice->name);
}

// ---------------------------------------------------------------------------
// State reset helpers
// ---------------------------------------------------------------------------

void PocketTTSImpl::resetFlowLMStates() {
    for (auto& buf : kvBufs_)   std::fill(buf.begin(),  buf.end(),  0.0f);
    for (auto& buf : stepBufs_) std::fill(buf.begin(),  buf.end(),  0LL);
}

void PocketTTSImpl::resetMimiDecStates() {
    for (size_t s = 0; s < mimiDecStateType_.size(); ++s) {
        switch (mimiDecStateType_[s]) {
        case MimiStateType::BOOL:
            std::fill(mimiDecStateBoolBufs_[s].begin(),  mimiDecStateBoolBufs_[s].end(),  false);
            break;
        case MimiStateType::INT64:
            std::fill(mimiDecStateInt64Bufs_[s].begin(), mimiDecStateInt64Bufs_[s].end(), 0LL);
            break;
        default:
            std::fill(mimiDecStateBufs_[s].begin(),      mimiDecStateBufs_[s].end(),      0.0f);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// appendFlowLMStateInputs
// Appends the 18 state input tensors to `dst` in slot order (0..17).
// ---------------------------------------------------------------------------

void PocketTTSImpl::appendFlowLMStateInputs(std::vector<Ort::Value>& dst) {
    int kvIdx = 0, stepIdx = 0;
    static const int64_t kv5dShape[] = {2, 1, 1000, 16, 64};
    static const int64_t stepShape[] = {1};
    static const int64_t emptyShape[]= {0};

    for (int s = 0; s < kNumFlowStates; ++s) {
        switch (flowStateType(s)) {
        case FlowStateType::KV_FLOAT:
            dst.push_back(Ort::Value::CreateTensor<float>(
                memInfo_, kvBufs_[kvIdx].data(), kKVElems,
                kv5dShape, 5));
            ++kvIdx;
            break;
        case FlowStateType::EMPTY_FLOAT:
            dst.push_back(Ort::Value::CreateTensor<float>(
                memInfo_, emptyFloatBuf_.data(), 0,
                emptyShape, 1));
            break;
        case FlowStateType::STEP_INT64:
            dst.push_back(Ort::Value::CreateTensor<int64_t>(
                memInfo_, stepBufs_[stepIdx].data(), 1,
                stepShape, 1));
            ++stepIdx;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// updateFlowLMStates
// Copy output state tensors back into the input buffers for the next step.
// `stateStartIdx` is the index of the first state in the outputs vector.
// ---------------------------------------------------------------------------

void PocketTTSImpl::updateFlowLMStates(const std::vector<Ort::Value>& outputs,
                                       size_t stateStartIdx) {
    int kvIdx = 0, stepIdx = 0;
    for (int s = 0; s < kNumFlowStates; ++s) {
        const size_t outIdx = stateStartIdx + static_cast<size_t>(s);
        if (outIdx >= outputs.size()) break;
        switch (flowStateType(s)) {
        case FlowStateType::KV_FLOAT: {
            const float* src = outputs[outIdx].GetTensorData<float>();
            std::memcpy(kvBufs_[kvIdx].data(), src, kKVElems * sizeof(float));
            ++kvIdx;
            break;
        }
        case FlowStateType::STEP_INT64: {
            const int64_t* src = outputs[outIdx].GetTensorData<int64_t>();
            stepBufs_[stepIdx][0] = src[0];
            ++stepIdx;
            break;
        }
        default: break; // EMPTY: nothing to copy
        }
    }
}

// ---------------------------------------------------------------------------
// runTextConditioner
// ---------------------------------------------------------------------------

std::vector<float> PocketTTSImpl::runTextConditioner(const std::vector<int64_t>& tokenIds) {
    const int64_t N = static_cast<int64_t>(tokenIds.size());
    const int64_t shape[2] = {1, N};

    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        memInfo_, const_cast<int64_t*>(tokenIds.data()), tokenIds.size(),
        shape, 2));

    std::vector<const char*> inPtrs  = {textCondInNames_[0].c_str()};
    std::vector<const char*> outPtrs = {textCondOutNames_[0].c_str()};

    auto outputs = textCondSession_->Run(
        Ort::RunOptions{}, inPtrs.data(), inputs.data(), 1,
        outPtrs.data(), 1);

    const float* data = outputs[0].GetTensorData<float>();
    const size_t elems = static_cast<size_t>(1 * N * 1024);
    return std::vector<float>(data, data + elems);
}

// ---------------------------------------------------------------------------
// runFlowLMMain
// ---------------------------------------------------------------------------

PocketTTSImpl::FlowLMMainResult PocketTTSImpl::runFlowLMMain(
    const float* seqData, int64_t seqLen,
    const float* textEmb,  int64_t embN,
    int64_t embDim)
{
    // Build inputs in order: sequence, text_embeddings, state_0..state_17
    std::vector<Ort::Value> inputs;
    inputs.reserve(2 + kNumFlowStates);

    // sequence: [1, seqLen, 32]
    // seqData points into a caller-owned buffer that lives until after Run() returns.
    {
        const int64_t shape[] = {1, seqLen, 32};
        const size_t  elems   = static_cast<size_t>(seqLen * 32);
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float*>(seqData), elems, shape, 3));
    }

    // text_embeddings: [1, embN, embDim]
    {
        const int64_t shape[] = {1, embN, embDim};
        const size_t  elems   = static_cast<size_t>(embN) * static_cast<size_t>(embDim);
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float*>(textEmb), elems, shape, 3));
    }

    // States 0..17
    appendFlowLMStateInputs(inputs);

    // Prepare name pointers
    std::vector<const char*> inPtrs, outPtrs;
    inPtrs.reserve(flowLmMainInNames_.size());
    outPtrs.reserve(flowLmMainOutNames_.size());
    for (const auto& n : flowLmMainInNames_)  inPtrs.push_back(n.c_str());
    for (const auto& n : flowLmMainOutNames_) outPtrs.push_back(n.c_str());

    std::vector<Ort::Value> outputs = flowLmMainSession_->Run(
        Ort::RunOptions{},
        inPtrs.data(),  inputs.data(),  inPtrs.size(),
        outPtrs.data(), outPtrs.size());

    // Output[0] = conditioning c [1, cDim] (cDim introspected from model output)
    // Output[1] = eos_logit [1]
    // Output[2..19] = updated states
    FlowLMMainResult result;

    {
        const float* cData = outputs[0].GetTensorData<float>();
        auto cShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t cElems = 1;
        for (auto d : cShape) cElems *= static_cast<size_t>(std::max(d, int64_t(0)));
        result.c.assign(cData, cData + cElems);
    }
    result.eosLogit = outputs[1].GetTensorData<float>()[0];

    // Copy updated states back to input buffers
    updateFlowLMStates(outputs, 2);

    return result;
}

// ---------------------------------------------------------------------------
// runFlowLMFlow  (ODE velocity step)
// ---------------------------------------------------------------------------

std::vector<float> PocketTTSImpl::runFlowLMFlow(
    const std::vector<float>& c, float s, float t, const std::vector<float>& x)
{
    // c: [1, cDim]   s: [1,1]   t: [1,1]   x: [1, 32]
    std::vector<Ort::Value> inputs;
    inputs.reserve(4);

    {
        const int64_t cDim    = static_cast<int64_t>(c.size());
        const int64_t shape[] = {1, cDim};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float*>(c.data()), c.size(), shape, 2));
    }
    // sVal / tVal must be kept alive until after Run() - do NOT scope them inside an
    // inner block, as ORT CreateTensor stores the data pointer without copying it.
    float sVal = s, tVal = t;
    {
        const int64_t shape[] = {1, 1};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, &sVal, 1, shape, 2));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, &tVal, 1, shape, 2));
    }
    {
        const int64_t shape[] = {1, 32};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float*>(x.data()), x.size(), shape, 2));
    }

    std::vector<const char*> inPtrs, outPtrs;
    for (const auto& n : flowLmFlowInNames_)  inPtrs.push_back(n.c_str());
    for (const auto& n : flowLmFlowOutNames_) outPtrs.push_back(n.c_str());

    auto outputs = flowLmFlowSession_->Run(
        Ort::RunOptions{},
        inPtrs.data(),  inputs.data(),  inPtrs.size(),
        outPtrs.data(), outPtrs.size());

    const float* vData = outputs[0].GetTensorData<float>();
    return std::vector<float>(vData, vData + 32);
}

// ---------------------------------------------------------------------------
// runMimiDecoder
// ---------------------------------------------------------------------------

std::vector<float> PocketTTSImpl::runMimiDecoder(const std::vector<float>& latents,
                                                  int64_t numFrames) {
    // latent: [1, numFrames, 32] + 56 state inputs → audio [1, samples] + updated states
    std::vector<Ort::Value> inputs;
    inputs.reserve(1 + mimiDecStateBufs_.size());
    // Temporary uint8 storage for bool tensors (must outlive Run())
    std::vector<std::vector<uint8_t>> uint8Storage_;

    {
        const int64_t shape[] = {1, numFrames, 32};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float*>(latents.data()), latents.size(), shape, 3));
    }

    for (size_t s = 0; s < mimiDecStateType_.size(); ++s) {
        auto& shape = mimiDecStateShapes_[s];
        switch (mimiDecStateType_[s]) {
        case MimiStateType::BOOL: {
            // ORT bool tensors are backed by uint8 (one byte per element)
            auto& boolBuf = mimiDecStateBoolBufs_[s];
            uint8Storage_.push_back(std::vector<uint8_t>(boolBuf.size()));
            auto& u8 = uint8Storage_.back();
            for (size_t i = 0; i < boolBuf.size(); ++i) u8[i] = boolBuf[i] ? 1 : 0;
            inputs.push_back(Ort::Value::CreateTensor(
                memInfo_, u8.data(), u8.size() * sizeof(uint8_t),
                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
            break;
        }
        case MimiStateType::INT64: {
            auto& buf = mimiDecStateInt64Bufs_[s];
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memInfo_, buf.data(), buf.size(),
                shape.data(), shape.size()));
            break;
        }
        default: {
            auto& buf = mimiDecStateBufs_[s];
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memInfo_, buf.data(), buf.size(),
                shape.data(), shape.size()));
            break;
        }
        }
    }

    std::vector<const char*> inPtrs, outPtrs;
    for (const auto& n : mimiDecInNames_)  inPtrs.push_back(n.c_str());
    for (const auto& n : mimiDecOutNames_) outPtrs.push_back(n.c_str());

    std::vector<Ort::Value> outputs;
    try {
        outputs = mimiDecSession_->Run(
            Ort::RunOptions{},
            inPtrs.data(),  inputs.data(),  inPtrs.size(),
            outPtrs.data(), outPtrs.size());
    } catch (const Ort::Exception& e) {
        spdlog::error("[PocketTTS] mimi_decoder Run failed: {}", e.what());
        return {};
    }

    // Output[0] = audio [1, samples]
    const float* audioData = outputs[0].GetTensorData<float>();
    auto audioShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    size_t audioElems = 1;
    for (auto d : audioShape) audioElems *= static_cast<size_t>(std::max(d, int64_t(0)));

    std::vector<float> audio(audioData, audioData + audioElems);

    // Copy updated decoder states back into buffers
    for (size_t s = 0; s < mimiDecStateType_.size(); ++s) {
        const size_t outIdx = 1 + s;
        if (outIdx >= outputs.size()) break;
        try {
            auto shape = outputs[outIdx].GetTensorTypeAndShapeInfo().GetShape();
            size_t count = 1;
            for (auto d : shape) count *= static_cast<size_t>(std::max(d, int64_t(0)));
            mimiDecStateShapes_[s].assign(shape.begin(), shape.end());
            switch (mimiDecStateType_[s]) {
            case MimiStateType::BOOL: {
                const uint8_t* src = reinterpret_cast<const uint8_t*>(
                    outputs[outIdx].GetTensorData<bool>());
                mimiDecStateBoolBufs_[s].resize(count);
                for (size_t i = 0; i < count; ++i) mimiDecStateBoolBufs_[s][i] = (src[i] != 0);
                break;
            }
            case MimiStateType::INT64: {
                const int64_t* src = outputs[outIdx].GetTensorData<int64_t>();
                mimiDecStateInt64Bufs_[s].resize(count);
                std::memcpy(mimiDecStateInt64Bufs_[s].data(), src, count * sizeof(int64_t));
                break;
            }
            default: {
                const float* src = outputs[outIdx].GetTensorData<float>();
                mimiDecStateBufs_[s].resize(count);
                std::memcpy(mimiDecStateBufs_[s].data(), src, count * sizeof(float));
                break;
            }
            }
        } catch (...) {}
    }

    return audio;
}

// ---------------------------------------------------------------------------
// synthesize  (main entry point)
// ---------------------------------------------------------------------------

void PocketTTSImpl::synthesize(const std::string& text,
                               const std::atomic<bool>& cancelFlag,
                               const AudioCallback& onChunk) {
    if (!initialized_) {
        spdlog::error("[PocketTTS] synthesize() called before initialize()");
        return;
    }

    const Params params = getParams();

    // Locate requested voice (fall back to first)
    const VoiceEntry* voice = nullptr;
    for (const auto& v : voices_) {
        if (v.name == params.voiceName) { voice = &v; break; }
    }
    if (!voice && !voices_.empty()) {
        voice = &voices_[0];
        spdlog::warn("[PocketTTS] Voice '{}' not found; using '{}'",
                     params.voiceName, voice->name);
    }
    if (!voice) {
        spdlog::error("[PocketTTS] No voices loaded");
        return;
    }

    // Normalise text (adds trailing period, capitalises first letter)
    const std::string normalized = normalizeText(text);
    if (normalized.empty()) return;

    // Sentence-aware chunking - matches JS splitIntoBestSentences:
    //   split normalised text at .!? boundaries, merge adjacent sentences
    //   into chunks whose token count stays within kMaxChunkTokens.
    const auto textChunks = chunkBySentences(normalized, kMaxChunkTokens);
    if (textChunks.empty()) return;
    spdlog::debug("[PocketTTS] {} sentence chunk(s)", textChunks.size());

    // JS mimi-decoder streaming constants
    static constexpr int kFadeSamples        = 480;   // 20 ms @ 24 kHz
    static constexpr int kFirstChunkFrames   = 3;     // JS: FIRST_CHUNK_FRAMES
    static constexpr int kNormalChunkFrames  = 12;    // JS: NORMAL_CHUNK_FRAMES
    static constexpr int kInterChunkSamples  = 6000;  // JS: CHUNK_GAP_SEC=0.25 s * 24 kHz

    for (size_t chunkIdx = 0; chunkIdx < textChunks.size(); ++chunkIdx) {
        if (cancelFlag.load()) return;
        const auto& chunkText = textChunks[chunkIdx];

        // Tokenise this sentence chunk
        const auto chunkIds = tokenize(chunkText);
        if (chunkIds.empty()) continue;
        spdlog::debug("[PocketTTS] Chunk {}/{}: {} tokens: \"{}\"",
                      chunkIdx + 1, textChunks.size(),
                      chunkIds.size(), chunkText.substr(0, 50));

        // Reset MIMI per chunk (JS: RESET_MIMI_STATE_EACH_CHUNK=true)
        resetMimiDecStates();

        // JS: flowLmState = cloneFlowState(baseFlowState)
        //   Restore cached voice-conditioned state; rebuild cache if voice changed.
        refreshVoiceState(voice);

        // Text conditioning on top of voice-conditioned state
        std::vector<float> textEmb = runTextConditioner(chunkIds);
        if (cancelFlag.load()) return;
        {
            static const float kEmptySeq[1] = {};
            runFlowLMMain(kEmptySeq, 0,
                          textEmb.data(), static_cast<int64_t>(chunkIds.size()), 1024);
        }
        if (cancelFlag.load()) return;

        // --- Autoregressive generation (matches Python _autoregressive_generation) ---
        std::vector<float> latentBatch;
        // Initial latent: all NaN - signals BOS to the flow LM
        std::vector<float> seqBuf(32, std::numeric_limits<float>::quiet_NaN());

        static const float kEmptyEmb[1] = {};
        // Noise std = sqrt(temperature)  (Python: std = temp**0.5, default 0.7)
        const float odeStd = std::sqrt(std::max(0.0f, params.temperature));
        const float dt     = 1.0f / static_cast<float>(params.lsdSteps);

        // Python prepare_text_prompt: <=4 words → guess 3, else 1; then +2.
        int wordCount = 0;
        {
            std::istringstream iss(chunkText);
            for (std::string w; iss >> w; ) ++wordCount;
        }
        const int framesAfterEos = (wordCount <= 4 ? 3 : 1) + 2;

        // Python _estimate_max_gen_len: ceil((tokens/3 + 2s) * 12.5 fps)
        const int maxFrames = std::min(kMaxFrames, static_cast<int>(std::ceil(
            (static_cast<double>(chunkIds.size()) / kTokensPerSecondEstimate +
             kGenSecondsPadding) * kFrameRate)));

        int  eosStep              = -1;
        bool isFirstDecodeOfChunk = true;
        bool chunkEnded           = false;

        for (int step = 0; step < maxFrames; ++step) {
            if (cancelFlag.load()) return;

            // Python emits exactly framesAfterEos latents starting at the EOS
            // frame, then stops - the stop-step latent is never decoded.
            if (eosStep >= 0 && step >= eosStep + framesAfterEos) {
                chunkEnded = true;
                break;
            }

            // AR step
            FlowLMMainResult ar = runFlowLMMain(seqBuf.data(), 1, kEmptyEmb, 0);
            if (cancelFlag.load()) return;

            if (eosStep < 0 && ar.eosLogit > kEosThreshold) {
                eosStep = step;
                spdlog::debug("[PocketTTS] EOS at frame {}", step);
            }

            // ODE: integrate flow_lm_flow from noise ~ N(0, sqrt(temp))
            std::vector<float> x(32);
            for (auto& v : x) v = normalDist_(rng_) * odeStd;
            for (int j = 0; j < params.lsdSteps; ++j) {
                if (cancelFlag.load()) return;
                const float s   = static_cast<float>(j)     * dt;
                const float t   = static_cast<float>(j + 1) * dt;
                auto flowDir    = runFlowLMFlow(ar.c, s, t, x);
                for (int k = 0; k < 32; ++k) x[k] += flowDir[k] * dt;
            }

            seqBuf = x;
            latentBatch.insert(latentBatch.end(), x.begin(), x.end());

            // Mimi batching:
            //   first decode of each chunk  → 3 frames  (low TTFB)
            //   subsequent                  → 12 frames (throughput)
            // Remaining frames are flushed after the loop.
            const int pending = static_cast<int>(latentBatch.size() / 32);
            int decodeSize = 0;
            if      ( isFirstDecodeOfChunk && pending >= kFirstChunkFrames)
                                                         decodeSize = kFirstChunkFrames;
            else if (!isFirstDecodeOfChunk && pending >= kNormalChunkFrames)
                                                         decodeSize = kNormalChunkFrames;

            if (decodeSize > 0) {
                const size_t decodeElems = static_cast<size_t>(decodeSize) * 32;
                std::vector<float> toDecodeLatents(
                    latentBatch.begin(),
                    latentBatch.begin() + static_cast<std::ptrdiff_t>(decodeElems));
                latentBatch.erase(
                    latentBatch.begin(),
                    latentBatch.begin() + static_cast<std::ptrdiff_t>(decodeElems));

                auto audio = runMimiDecoder(toDecodeLatents, static_cast<int64_t>(decodeSize));
                if (!audio.empty()) {
                    // JS: applyFadeIn on first audio of each text chunk (chunkStart)
                    if (isFirstDecodeOfChunk) {
                        const int fade = std::min(kFadeSamples, static_cast<int>(audio.size()));
                        for (int i = 0; i < fade; ++i)
                            audio[i] *= static_cast<float>(i) / static_cast<float>(fade);
                        isFirstDecodeOfChunk = false;
                    }
                    if (!cancelFlag.load()) onChunk(std::move(audio));
                }
            }
        }

        // Python warns here: hitting max length without EOS usually indicates a problem.
        if (!chunkEnded)
            spdlog::warn("[PocketTTS] Max generation length ({} frames) reached without EOS "
                         "for chunk {}/{}", maxFrames, chunkIdx + 1, textChunks.size());

        if (cancelFlag.load()) return;

        // Flush remaining latents
        if (!latentBatch.empty()) {
            const int64_t batchFrames = static_cast<int64_t>(latentBatch.size() / 32);
            auto audio = runMimiDecoder(latentBatch, batchFrames);
            if (!audio.empty() && !cancelFlag.load()) {
                if (isFirstDecodeOfChunk) {
                    const int fade = std::min(kFadeSamples, static_cast<int>(audio.size()));
                    for (int i = 0; i < fade; ++i)
                        audio[i] *= static_cast<float>(i) / static_cast<float>(fade);
                }
                onChunk(std::move(audio));
            }
        }

        // Inter-chunk silence (JS: CHUNK_GAP_SEC=0.25 between sentence chunks)
        if (chunkEnded && chunkIdx + 1 < textChunks.size() && !cancelFlag.load())
            onChunk(std::vector<float>(kInterChunkSamples, 0.0f));
    }

    // Trailing silence
    if (params.silenceDuration > 0.0f && !cancelFlag.load()) {
        const int silenceSamples = static_cast<int>(24000 * params.silenceDuration);
        onChunk(std::vector<float>(silenceSamples, 0.0f));
    }
}

// ---------------------------------------------------------------------------
// Text normalisation
// ---------------------------------------------------------------------------

static const char* const kOnes[] = {
    "", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"
};
static const char* const kTens[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
};

static std::string numberToWords(long long n) {
    if (n == 0)  return "zero";
    if (n <  0)  return "negative " + numberToWords(-n);
    std::string r;
    if (n >= 1000000000LL) { r += numberToWords(n / 1000000000LL) + " billion "; n %= 1000000000LL; }
    if (n >= 1000000LL)    { r += numberToWords(n / 1000000LL)    + " million "; n %= 1000000LL; }
    if (n >= 1000LL)       { r += numberToWords(n / 1000LL)       + " thousand "; n %= 1000LL; }
    if (n >= 100)          { r += std::string(kOnes[n / 100]) + " hundred "; n %= 100; }
    if (n >= 20)  { r += kTens[n / 10]; if (n % 10) r += std::string(" ") + kOnes[n % 10]; }
    else if (n > 0) r += kOnes[n];
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

static std::string digitByDigit(const std::string& digits) {
    static const char* const kDigitNames[] = {
        "zero","one","two","three","four","five","six","seven","eight","nine"
    };
    std::string r;
    for (char c : digits) {
        if (c >= '0' && c <= '9') {
            if (!r.empty()) r += ' ';
            r += kDigitNames[c - '0'];
        }
    }
    return r;
}

/*static*/ std::string PocketTTSImpl::stripMarkdown(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '*' || c == '`') { ++i; continue; }
        if ((c == '-' || c == '\xE2') && (i == 0 || text[i-1] == '\n')) {
            if (c == '\xE2' && i+2 < text.size() &&
                (unsigned char)text[i+1] == 0x80 && (unsigned char)text[i+2] == 0xA2) {
                i += 3; if (i < text.size() && text[i] == ' ') ++i; continue;
            }
            if (c == '-' && i+1 < text.size() && text[i+1] == ' ') { i += 2; continue; }
        }
        if (c == '\n' || c == '\r') {
            if (!result.empty() && result.back() != ' ') result += ' ';
            ++i; continue;
        }
        if (std::isdigit((unsigned char)c) && (i == 0 || text[i-1] == '\n')) {
            size_t j = i;
            while (j < text.size() && std::isdigit((unsigned char)text[j])) ++j;
            if (j < text.size() && (text[j] == '.' || text[j] == ')') &&
                j+1 < text.size() && text[j+1] == ' ') { i = j+2; continue; }
        }
        result += c; ++i;
    }
    std::string cleaned;
    cleaned.reserve(result.size());
    bool lastSpace = false;
    for (char ch : result) {
        if (ch == ' ') { if (!lastSpace) cleaned += ch; lastSpace = true; }
        else { cleaned += ch; lastSpace = false; }
    }
    size_t a = cleaned.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = cleaned.find_last_not_of(' ');
    return cleaned.substr(a, b - a + 1);
}

/*static*/ std::string PocketTTSImpl::expandNumbers(const std::string& text) {
    std::string result;
    result.reserve(text.size() * 2);
    size_t i = 0;
    while (i < text.size()) {
        if (std::isdigit((unsigned char)text[i])) {
            std::string token;
            while (i < text.size() &&
                   (std::isdigit((unsigned char)text[i]) || text[i] == '-' || text[i] == '.')) {
                token += text[i]; ++i;
            }
            while (!token.empty() && !std::isdigit((unsigned char)token.back())) {
                token.pop_back(); --i;
            }
            bool hasHyphen = token.find('-') != std::string::npos;
            bool hasDot    = token.find('.') != std::string::npos;
            if (hasHyphen) {
                std::string spoken, group;
                for (char c : token) {
                    if (c == '-') { if (!group.empty()) { if (!spoken.empty()) spoken += ", "; spoken += digitByDigit(group); group.clear(); } }
                    else group += c;
                }
                if (!group.empty()) { if (!spoken.empty()) spoken += ", "; spoken += digitByDigit(group); }
                result += spoken;
            } else if (hasDot) {
                auto dot = token.find('.');
                long long intVal = 0; try { intVal = std::stoll(token.substr(0, dot)); } catch (...) {}
                result += numberToWords(intVal) + " point " + digitByDigit(token.substr(dot + 1));
            } else {
                long long val = 0; try { val = std::stoll(token); } catch (...) {}
                result += (token.size() > 7) ? digitByDigit(token) : numberToWords(val);
            }
        } else {
            result += text[i]; ++i;
        }
    }
    return result;
}

/*static*/ std::string PocketTTSImpl::normalizeText(const std::string& text) {
    std::string result = expandNumbers(stripMarkdown(text));
    // Trim whitespace
    size_t l = 0;
    while (l < result.size() && std::isspace((unsigned char)result[l]))    ++l;
    size_t r = result.size();
    while (r > l  && std::isspace((unsigned char)result[r - 1]))           --r;
    result = result.substr(l, r - l);
    if (result.empty()) return result;
    // Capitalise first character (JS: prepareText capitalises first letter)
    if (std::islower((unsigned char)result[0]))
        result[0] = static_cast<char>(std::toupper((unsigned char)result[0]));
    // Ensure trailing period if text ends without sentence terminator (JS behaviour)
    if (std::isalnum((unsigned char)result.back()))
        result += '.';
    return result;
}
