#include "MossTTSImpl.hpp"
#include "MossTextNormalizer.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// UTF-8 / text helpers
// ---------------------------------------------------------------------------

using moss::text::detail::toU32;
using moss::text::detail::toUtf8;

static bool isSpace(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == U'\v' ||
           c == 0x3000 || c == 0x00A0;
}

static std::u32string strip(const std::u32string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a]))     ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static bool inSet(char32_t c, std::u32string_view set) {
    return set.find(c) != std::u32string_view::npos;
}

// Punctuation sets from onnx_tts_runtime.py.
static std::u32string_view sentenceEndChars() { return U".!?。！？；;"; }       // SENTENCE_END_PUNCTUATION
static std::u32string_view clauseSplitChars() { return U",，、；;：:"; }         // CLAUSE_SPLIT_PUNCTUATION
static std::u32string_view closingChars()     { return U"\"'”’)]}）】》」』"; } // CLOSING_PUNCTUATION

static bool isCjk(char32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x3040 && c <= 0x30FF) || (c >= 0xAC00 && c <= 0xD7AF);
}

static bool containsCjk(const std::u32string& s) {
    return std::any_of(s.begin(), s.end(), isCjk);
}

// Approximates Python str.isalnum() for the scripts MOSS-TTS-Nano covers.
static bool isAlnum(char32_t c) {
    if (c < 0x80) return std::isalnum(static_cast<int>(c)) != 0;
    return (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) || // Latin-1 / Extended
           (c >= 0x0370 && c <= 0x03FF) ||                                // Greek
           (c >= 0x0400 && c <= 0x04FF) ||                                // Cyrillic
           isCjk(c);
}

static int countWords(const std::u32string& s) {
    int n = 0;
    bool inWord = false;
    for (char32_t c : s) {
        if (isSpace(c)) inWord = false;
        else if (!inWord) { inWord = true; ++n; }
    }
    return n;
}

// _prepare_text_for_sentence_chunking
static std::u32string prepareForChunking(std::u32string t) {
    t = strip(t);
    if (t.empty()) return t;
    for (auto& c : t) if (c == U'\r' || c == U'\n') c = U' ';
    std::u32string collapsed;
    for (char32_t c : t) {
        if (c == U' ' && !collapsed.empty() && collapsed.back() == U' ') continue;
        collapsed += c;
    }
    t = std::move(collapsed);
    if (containsCjk(t)) {
        if (!inSet(t.back(), sentenceEndChars())) t += U'。';
        return t;
    }
    if (t[0] >= U'a' && t[0] <= U'z') t[0] = t[0] - U'a' + U'A';
    if (isAlnum(t.back())) t += U'.';
    if (countWords(t) < 5) t = U"        " + t;
    return t;
}

// _split_text_by_punctuation
static std::vector<std::u32string> splitByPunctuation(const std::u32string& text,
                                                      std::u32string_view punctuation) {
    std::vector<std::u32string> out;
    std::u32string cur;
    size_t i = 0;
    while (i < text.size()) {
        const char32_t c = text[i];
        cur += c;
        if (inSet(c, punctuation)) {
            size_t j = i + 1;
            while (j < text.size() && inSet(text[j], closingChars())) cur += text[j++];
            auto s = strip(cur);
            if (!s.empty()) out.push_back(std::move(s));
            cur.clear();
            while (j < text.size() && isSpace(text[j])) ++j;
            i = j;
            continue;
        }
        ++i;
    }
    auto tail = strip(cur);
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

static std::u32string joinParts(const std::u32string& l, const std::u32string& r) {
    if (l.empty()) return r;
    if (r.empty()) return l;
    if (containsCjk(l) || containsCjk(r)) return l + r;
    return l + U" " + r;
}

static double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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

// Ort::Value has no default constructor; Session::Run takes a contiguous array of inputs.
static std::vector<Ort::Value> nullValues(size_t n) {
    std::vector<Ort::Value> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) v.emplace_back(nullptr);
    return v;
}

static std::vector<const char*> cstrs(const std::vector<std::string>& v) {
    std::vector<const char*> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = v[i].c_str();
    return out;
}

static bool indexOf(const std::vector<std::string>& names, const std::string& name, size_t& idx) {
    auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return false;
    idx = static_cast<size_t>(it - names.begin());
    return true;
}

static bool readJson(const fs::path& path, nlohmann::json& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        spdlog::error("[MossTTS] Cannot open {}", path.string());
        return false;
    }
    try {
        out = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        spdlog::error("[MossTTS] Failed to parse {}: {}", path.string(), e.what());
        return false;
    }
    return true;
}

static std::vector<int32_t> toIntVector(const nlohmann::json& j) {
    std::vector<int32_t> v;
    v.reserve(j.size());
    for (const auto& x : j) v.push_back(x.get<int32_t>());
    return v;
}

// ---------------------------------------------------------------------------
// Constructor / params
// ---------------------------------------------------------------------------

MossTTSImpl::MossTTSImpl(const std::string& modelDir, const std::string& voiceName, int numThreads)
    : modelDir_(modelDir)
    , numThreads_(std::max(1, numThreads))
    , rng_(std::random_device{}())
{
    params_.voiceName = voiceName;
}

void MossTTSImpl::setParams(const Params& p) {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    params_ = p;
}

MossTTSImpl::Params MossTTSImpl::getParams() const {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    return params_;
}

std::vector<std::string> MossTTSImpl::getVoiceNames() const {
    std::vector<std::string> names;
    names.reserve(voices_.size());
    for (const auto& v : voices_) names.push_back(v.name);
    return names;
}

const MossTTSImpl::Voice* MossTTSImpl::findVoice(const std::string& name) const {
    for (const auto& v : voices_)
        if (v.name == name) return &v;
    return voices_.empty() ? nullptr : &voices_.front();
}

// ---------------------------------------------------------------------------
// initialize()
// ---------------------------------------------------------------------------

bool MossTTSImpl::initialize() {
    if (initialized_) return true;

    sessionOpts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOpts_.SetIntraOpNumThreads(numThreads_);
    sessionOpts_.SetInterOpNumThreads(1);

    std::string ttsDir, codecDir, codecStepFile, prefillFile, decodeFile, localFrameFile;
    if (!loadManifest(ttsDir, codecDir, codecStepFile, prefillFile, decodeFile, localFrameFile))
        return false;
    if (!loadModels(ttsDir, codecDir, prefillFile, decodeFile, localFrameFile, codecStepFile))
        return false;
    if (!resolveIoBindings())
        return false;

    initialized_ = true;
    spdlog::info("[MossTTS] Initialized. Voices: {}, SampleRate: {} Hz, threads: {}",
                 voices_.size(), sampleRate_, numThreads_);
    return true;
}

bool MossTTSImpl::loadManifest(std::string& ttsDir, std::string& codecDir, std::string& codecStepFile,
                               std::string& prefillFile, std::string& decodeFile,
                               std::string& localFrameFile) {
    const fs::path root(modelDir_);
    fs::path manifestPath;
    const char* const manifestCandidates[] = {
        "browser_poc_manifest.json",
        "MOSS-TTS-Nano-100M-ONNX/browser_poc_manifest.json",
    };
    for (const char* c : manifestCandidates) {
        if (fs::is_regular_file(root / c)) { manifestPath = root / c; break; }
    }
    if (manifestPath.empty()) {
        spdlog::error("[MossTTS] browser_poc_manifest.json not found under {}", modelDir_);
        return false;
    }
    const fs::path manifestDir = manifestPath.parent_path();

    nlohmann::json manifest, ttsMeta, codecMeta;
    if (!readJson(manifestPath, manifest)) return false;

    try {
        const auto& files = manifest.at("model_files");
        const fs::path ttsMetaPath = (manifestDir / files.at("tts_meta").get<std::string>()).lexically_normal();
        fs::path codecMetaPath = (manifestDir / files.at("codec_meta").get<std::string>()).lexically_normal();
        if (!fs::is_regular_file(codecMetaPath)) codecMetaPath = root / "MOSS-Audio-Tokenizer-Nano-ONNX" / "codec_browser_onnx_meta.json";
        const fs::path tokenizerPath =
            manifestDir / files.value("tokenizer_model", std::string("tokenizer.model"));

        if (!readJson(ttsMetaPath, ttsMeta))     return false;
        if (!readJson(codecMetaPath, codecMeta)) return false;

        // tts_config
        const auto& cfg = manifest.at("tts_config");
        nVq_                       = cfg.at("n_vq").get<int>();
        rowWidth_                  = nVq_ + 1;
        audioPadTokenId_           = cfg.at("audio_pad_token_id").get<int>();
        audioStartTokenId_         = cfg.at("audio_start_token_id").get<int>();
        audioEndTokenId_           = cfg.at("audio_end_token_id").get<int>();
        audioUserSlotTokenId_      = cfg.value("audio_user_slot_token_id", 8);
        audioAssistantSlotTokenId_ = cfg.at("audio_assistant_slot_token_id").get<int>();
        codebookSize_              = cfg.at("audio_codebook_sizes").at(0).get<int>();

        const auto& tpl = manifest.at("prompt_templates");
        userPromptPrefixIds_         = toIntVector(tpl.at("user_prompt_prefix_token_ids"));
        userPromptAfterReferenceIds_ = toIntVector(tpl.at("user_prompt_after_reference_token_ids"));
        assistantPromptPrefixIds_    = toIntVector(tpl.at("assistant_prompt_prefix_token_ids"));

        if (manifest.contains("generation_defaults"))
            manifestMaxNewFrames_ = manifest["generation_defaults"].value("max_new_frames", 375);

        voices_.clear();
        for (const auto& v : manifest.value("builtin_voices", nlohmann::json::array())) {
            if (!v.contains("prompt_audio_codes") || v["prompt_audio_codes"].empty()) continue;
            Voice voice;
            voice.name    = v.value("voice", std::string());
            voice.english = v.value("group", std::string()).rfind("English", 0) == 0;
            for (const auto& row : v["prompt_audio_codes"]) voice.promptAudioCodes.push_back(toIntVector(row));
            voices_.push_back(std::move(voice));
        }
        if (voices_.empty()) {
            spdlog::error("[MossTTS] No built-in voices with prompt_audio_codes in {}", manifestPath.string());
            return false;
        }

        // TTS graph files
        const auto& ttsFiles = ttsMeta.at("files");
        ttsDir         = ttsMetaPath.parent_path().string();
        prefillFile    = ttsFiles.at("prefill").get<std::string>();
        decodeFile     = ttsFiles.at("decode_step").get<std::string>();
        localFrameFile = ttsFiles.at("local_fixed_sampled_frame").get<std::string>();

        // Codec: streaming decoder and its state tensors
        codecDir      = codecMetaPath.parent_path().string();
        codecStepFile = codecMeta.at("files").at("decode_step").get<std::string>();
        sampleRate_   = codecMeta.at("codec_config").at("sample_rate").get<int>();

        // Offsets start at 0, KV caches at 0.0, cached positions at -1 (CodecStreamingDecodeSession.reset).
        codecStateSpecs_.clear();
        auto addState = [&](const nlohmann::json& j, const char* in, const char* out, const char* shape,
                            bool isFloat, int32_t fill) {
            CodecStateSpec s;
            s.inName  = j.at(in).get<std::string>();
            s.outName = j.at(out).get<std::string>();
            s.shape   = j.at(shape).get<std::vector<int64_t>>();
            s.isFloat = isFloat;
            s.fillInt = fill;
            codecStateSpecs_.push_back(std::move(s));
        };
        const auto& sd = codecMeta.at("streaming_decode");
        for (const auto& t : sd.at("transformer_offsets"))
            addState(t, "input_name", "output_name", "shape", false, 0);
        for (const auto& a : sd.at("attention_caches")) {
            addState(a, "offset_input_name",           "offset_output_name",           "offset_shape",    false,  0);
            addState(a, "cached_keys_input_name",      "cached_keys_output_name",      "cache_shape",     true,   0);
            addState(a, "cached_values_input_name",    "cached_values_output_name",    "cache_shape",     true,   0);
            addState(a, "cached_positions_input_name", "cached_positions_output_name", "positions_shape", false, -1);
        }

        // Tokenizer
        tokenizer_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
        const auto status = tokenizer_->Load(tokenizerPath.string());
        if (!status.ok()) {
            spdlog::error("[MossTTS] SentencePiece load failed ({}): {}", tokenizerPath.string(), status.ToString());
            return false;
        }
    } catch (const std::exception& e) {
        spdlog::error("[MossTTS] Invalid model metadata: {}", e.what());
        return false;
    }
    return true;
}

bool MossTTSImpl::loadModels(const std::string& ttsDir, const std::string& codecDir,
                             const std::string& prefillFile, const std::string& decodeFile,
                             const std::string& localFrameFile, const std::string& codecStepFile) {
    struct Spec { fs::path path; std::unique_ptr<Ort::Session>* session; };
    const Spec specs[] = {
        { fs::path(ttsDir)   / prefillFile,    &prefillSession_    },
        { fs::path(ttsDir)   / decodeFile,     &decodeSession_     },
        { fs::path(ttsDir)   / localFrameFile, &localFrameSession_ },
        { fs::path(codecDir) / codecStepFile,  &codecStepSession_  },
    };
    for (const auto& spec : specs) {
        if (!fs::is_regular_file(spec.path)) {
            spdlog::error("[MossTTS] Model not found: {}", spec.path.string());
            return false;
        }
        try {
            // path::c_str() is wchar_t* on Windows, matching ORTCHAR_T.
            *spec.session = std::make_unique<Ort::Session>(env_, spec.path.c_str(), sessionOpts_);
        } catch (const Ort::Exception& e) {
            spdlog::error("[MossTTS] Failed to load {}: {}", spec.path.string(), e.what());
            return false;
        }
        spdlog::info("[MossTTS] Loaded {}", spec.path.filename().string());
    }

    prefillInNames_  = fetchNames(*prefillSession_,    true);
    prefillOutNames_ = fetchNames(*prefillSession_,    false);
    decodeInNames_   = fetchNames(*decodeSession_,     true);
    decodeOutNames_  = fetchNames(*decodeSession_,     false);
    localInNames_    = fetchNames(*localFrameSession_, true);
    localOutNames_   = fetchNames(*localFrameSession_, false);
    codecInNames_    = fetchNames(*codecStepSession_,  true);
    codecOutNames_   = fetchNames(*codecStepSession_,  false);
    return true;
}

bool MossTTSImpl::resolveIoBindings() {
    if (!indexOf(prefillOutNames_, "global_hidden", prefillHiddenOutIdx_) ||
        !indexOf(decodeOutNames_,  "global_hidden", decodeHiddenOutIdx_)) {
        spdlog::error("[MossTTS] global_hidden output missing from prefill/decode graph");
        return false;
    }

    kvDecodeInIdx_.clear();
    kvPrefillOutIdx_.clear();
    kvDecodeOutIdx_.clear();
    for (size_t i = 0; i < decodeInNames_.size(); ++i) {
        const auto& name = decodeInNames_[i];
        if (name.rfind("past_", 0) != 0 || name == "past_valid_lengths") continue;
        const std::string present = "present_" + name.substr(5);
        size_t pIdx = 0, dIdx = 0;
        if (!indexOf(prefillOutNames_, present, pIdx) || !indexOf(decodeOutNames_, present, dIdx)) {
            spdlog::error("[MossTTS] No '{}' output for decode input '{}'", present, name);
            return false;
        }
        kvDecodeInIdx_.push_back(i);
        kvPrefillOutIdx_.push_back(pIdx);
        kvDecodeOutIdx_.push_back(dIdx);
    }

    for (auto& s : codecStateSpecs_) {
        if (!indexOf(codecInNames_, s.inName, s.inIdx) || !indexOf(codecOutNames_, s.outName, s.outIdx)) {
            spdlog::error("[MossTTS] Codec state '{}' -> '{}' not found in codec graph", s.inName, s.outName);
            return false;
        }
    }
    if (codecStateSpecs_.size() + 2 != codecInNames_.size()) {
        spdlog::error("[MossTTS] Codec graph has {} inputs; metadata describes {} states",
                      codecInNames_.size(), codecStateSpecs_.size());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tokenisation / chunking (split_voice_clone_text)
// ---------------------------------------------------------------------------

std::vector<int32_t> MossTTSImpl::tokenize(const std::string& text) const {
    std::vector<int> ids;
    tokenizer_->Encode(text, &ids);
    return std::vector<int32_t>(ids.begin(), ids.end());
}

int MossTTSImpl::countTokens(const std::string& text) const {
    return static_cast<int>(tokenize(text).size());
}

std::vector<std::string> MossTTSImpl::splitByTokenBudget(const std::u32string& text, int maxTokens) const {
    std::vector<std::string> pieces;
    std::u32string remaining = strip(text);
    const std::u32string boundary = std::u32string(clauseSplitChars()) + std::u32string(sentenceEndChars()) + U" ";
    while (!remaining.empty()) {
        if (countTokens(toUtf8(remaining)) <= maxTokens) {
            pieces.push_back(toUtf8(remaining));
            break;
        }
        // Longest prefix that fits the budget.
        size_t low = 1, high = remaining.size(), best = 1;
        while (low <= high) {
            const size_t mid = (low + high) / 2;
            const auto candidate = strip(remaining.substr(0, mid));
            if (candidate.empty()) { low = mid + 1; continue; }
            if (countTokens(toUtf8(candidate)) <= maxTokens) { best = mid; low = mid + 1; }
            else                                             { high = mid - 1; }
        }
        // Prefer a boundary within the last 25 characters of that prefix.
        size_t cut = best;
        const size_t scanMin = best > 25 ? best - 25 : 0;
        for (size_t k = best; k > scanMin; --k) {
            if (inSet(remaining[k - 1], boundary)) { cut = k; break; }
        }
        auto piece = strip(remaining.substr(0, cut));
        if (piece.empty()) {
            piece = strip(remaining.substr(0, best));
            cut   = best;
        }
        pieces.push_back(toUtf8(piece));
        remaining = strip(remaining.substr(cut));
    }
    return pieces;
}

std::vector<std::string> MossTTSImpl::splitIntoChunks(const std::string& text, int maxTokens) const {
    const std::u32string original = strip(toU32(text));
    if (original.empty()) return {};
    maxTokens = std::max(1, maxTokens);

    const std::u32string prepared = prepareForChunking(original);
    auto sentences = splitByPunctuation(prepared, sentenceEndChars());
    if (sentences.empty()) sentences.push_back(strip(prepared));

    std::vector<std::pair<int, std::u32string>> slices;
    for (const auto& sentence : sentences) {
        const int n = countTokens(toUtf8(sentence));
        if (n <= maxTokens) { slices.emplace_back(n, sentence); continue; }
        auto clauses = splitByPunctuation(sentence, clauseSplitChars());
        if (clauses.size() <= 1) clauses = {sentence};
        for (const auto& clause : clauses) {
            const int cn = countTokens(toUtf8(clause));
            if (cn <= maxTokens) { slices.emplace_back(cn, clause); continue; }
            for (const auto& piece : splitByTokenBudget(clause, maxTokens))
                slices.emplace_back(countTokens(piece), toU32(piece));
        }
    }

    std::vector<std::string> chunks;
    std::u32string cur;
    int curTokens = 0;
    for (const auto& [n, s] : slices) {
        if (cur.empty()) { cur = s; curTokens = n; continue; }
        if (curTokens + n > maxTokens) {
            chunks.push_back(toUtf8(strip(cur)));
            cur = s;
            curTokens = n;
        } else {
            cur = joinParts(cur, s);
            curTokens = countTokens(toUtf8(cur));
        }
    }
    if (!cur.empty()) chunks.push_back(toUtf8(strip(cur)));

    // Upstream synthesises the original (unprepared) text when it fits in one chunk.
    if (chunks.size() <= 1) return {toUtf8(original)};
    return chunks;
}

// ---------------------------------------------------------------------------
// Text normalisation
// ---------------------------------------------------------------------------

/*static*/ std::string MossTTSImpl::stripMarkdown(const std::string& text) {
    // Strip markdown emphasis/code markers, heading hashes and list bullets; flatten newlines.
    std::string result;
    result.reserve(text.size());
    bool lineStart = true;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            if (!result.empty() && result.back() != ' ') result += ' ';
            lineStart = true;
            continue;
        }
        if (c == '*' || c == '`') continue;
        if (lineStart) {
            if (c == ' ' || c == '\t') continue;
            if (c == '#') { while (i + 1 < text.size() && text[i + 1] == '#') ++i; continue; }
            if ((c == '-' || c == '+') && i + 1 < text.size() && text[i + 1] == ' ') { ++i; continue; }
            if (c == '\xE2' && i + 2 < text.size() &&
                static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                static_cast<unsigned char>(text[i + 2]) == 0xA2) { i += 2; continue; } // •
        }
        lineStart = false;
        if (c == '\t') { result += ' '; continue; }
        result += c;
    }
    std::string cleaned;
    cleaned.reserve(result.size());
    for (char ch : result) {
        if (ch == ' ' && (cleaned.empty() || cleaned.back() == ' ')) continue;
        cleaned += ch;
    }
    while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    return cleaned;
}

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------

void MossTTSImpl::emit(std::vector<float> pcm, StreamClock& clock, const AudioCallback& onChunk) const {
    if (pcm.empty()) return;
    if (!clock.started) {
        clock.started      = true;
        clock.startSeconds = nowSeconds();
    }
    clock.emittedSamples += pcm.size();
    onChunk(std::move(pcm));
}

void MossTTSImpl::synthesize(const std::string& text,
                             const std::atomic<bool>& cancelFlag,
                             const AudioCallback& onChunk) {
    if (!initialized_) throw std::runtime_error("MossTTS not initialized");

    const Params params = getParams();
    const Voice* voice  = findVoice(params.voiceName);
    if (!voice) throw std::runtime_error("No MOSS voice available");
    if (voice->name != params.voiceName)
        spdlog::warn("[MossTTS] Voice '{}' not found; using '{}'", params.voiceName, voice->name);

    const std::string normalized = params.normalizeText ? moss::text::normalize(text, voice->english)
                                                        : stripMarkdown(text);
    if (normalized.empty()) return;
    spdlog::debug("[MossTTS] normalized: {}", normalized);

    const int maxFrames = params.maxNewFrames > 0 ? params.maxNewFrames : manifestMaxNewFrames_;
    const auto chunks   = splitIntoChunks(normalized, params.maxChunkTokens);

    StreamClock clock;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (cancelFlag.load()) return;
        const auto ids = tokenize(chunks[i]);
        if (ids.empty()) continue;

        const double t0 = nowSeconds();
        const size_t before = clock.emittedSamples;
        if (!synthesizeChunk(ids, *voice, maxFrames, clock, cancelFlag, onChunk)) return;
        const double audioSec = static_cast<double>(clock.emittedSamples - before) / sampleRate_;
        const double elapsed  = nowSeconds() - t0;
        spdlog::debug("[MossTTS] chunk {}/{}: {} tokens, {:.2f}s audio in {:.2f}s (RTF {:.2f})",
                      i + 1, chunks.size(), ids.size(), audioSec, elapsed,
                      audioSec > 0 ? elapsed / audioSec : 0.0);

        if (i + 1 < chunks.size()) {
            const float pause = countWords(toU32(chunks[i])) <= 4 ? params.pauseShortSeconds : params.pauseLongSeconds;
            emit(std::vector<float>(static_cast<size_t>(sampleRate_ * pause), 0.0f), clock, onChunk);
        }
    }

    if (params.silenceDuration > 0.0f && !cancelFlag.load())
        emit(std::vector<float>(static_cast<size_t>(sampleRate_ * params.silenceDuration), 0.0f), clock, onChunk);
}

bool MossTTSImpl::synthesizeChunk(const std::vector<int32_t>& textIds, const Voice& voice, int maxFrames,
                                  StreamClock& clock, const std::atomic<bool>& cancelFlag,
                                  const AudioCallback& onChunk) {
    // --- Prompt rows: [text prefix][voice audio codes][text suffix], each row nVq + 1 wide ---
    std::vector<int32_t> rows;
    auto pushText = [&](int32_t token) {
        rows.push_back(token);
        rows.insert(rows.end(), static_cast<size_t>(nVq_), audioPadTokenId_);
    };
    for (int32_t t : userPromptPrefixIds_) pushText(t);
    pushText(audioStartTokenId_);
    for (const auto& code : voice.promptAudioCodes) {
        rows.push_back(audioUserSlotTokenId_);
        for (int q = 0; q < nVq_; ++q)
            rows.push_back(q < static_cast<int>(code.size()) ? code[q] : audioPadTokenId_);
    }
    pushText(audioEndTokenId_);
    for (int32_t t : userPromptAfterReferenceIds_) pushText(t);
    for (int32_t t : textIds)                      pushText(t);
    for (int32_t t : assistantPromptPrefixIds_)    pushText(t);
    pushText(audioStartTokenId_);

    const int64_t seqLen = static_cast<int64_t>(rows.size() / rowWidth_);
    std::vector<int32_t> mask(static_cast<size_t>(seqLen), 1);

    // --- Prefill ---
    std::vector<Ort::Value> outputs;
    {
        const int64_t idsShape[]  = {1, seqLen, rowWidth_};
        const int64_t maskShape[] = {1, seqLen};
        auto inputs = nullValues(prefillInNames_.size());
        for (size_t i = 0; i < prefillInNames_.size(); ++i) {
            if (prefillInNames_[i] == "input_ids")
                inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, rows.data(), rows.size(), idsShape, 3);
            else if (prefillInNames_[i] == "attention_mask")
                inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, mask.data(), mask.size(), maskShape, 2);
            else
                throw std::runtime_error("Unexpected prefill input: " + prefillInNames_[i]);
        }
        const auto inNames  = cstrs(prefillInNames_);
        const auto outNames = cstrs(prefillOutNames_);
        outputs = prefillSession_->Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
                                       outNames.data(), outNames.size());
    }

    auto lastHidden = [](Ort::Value& v) {
        const auto shape = v.GetTensorTypeAndShapeInfo().GetShape();
        const int64_t hidden = shape.back();
        const int64_t rowsN  = shape.size() == 3 ? shape[1] : 1;
        const float* p = v.GetTensorData<float>() + (rowsN - 1) * hidden;
        return std::vector<float>(p, p + hidden);
    };

    std::vector<float> hidden = lastHidden(outputs[prefillHiddenOutIdx_]);
    std::vector<Ort::Value> kv;
    kv.reserve(kvDecodeInIdx_.size());
    for (size_t k = 0; k < kvDecodeInIdx_.size(); ++k) kv.push_back(std::move(outputs[kvPrefillOutIdx_[k]]));
    outputs.clear();
    int32_t pastValidLength = static_cast<int32_t>(seqLen);

    // --- Autoregressive frame loop ---
    std::vector<int32_t> seenMask(static_cast<size_t>(nVq_) * codebookSize_, 0);
    std::uniform_real_distribution<float> uniform(1e-6f, 1.0f - 1e-6f);
    std::vector<float> assistantU(1), audioU(static_cast<size_t>(nVq_));
    std::vector<int32_t> pending;   // frames awaiting codec decode, [n][nVq]
    std::vector<int32_t> stepRow(static_cast<size_t>(rowWidth_));
    resetCodecState();

    const auto localIn   = cstrs(localInNames_);
    const auto localOut  = cstrs(localOutNames_);
    const auto decodeIn  = cstrs(decodeInNames_);
    const auto decodeOut = cstrs(decodeOutNames_);

    // Decode in small batches while the playback lead is short (first audio fast), larger
    // ones once audio is well ahead of real time (_resolve_stream_decode_frame_budget).
    auto decodeBudget = [&]() {
        if (!clock.started) return 1;
        const double lead = static_cast<double>(clock.emittedSamples) / sampleRate_ -
                            (nowSeconds() - clock.startSeconds);
        if (lead < 0.20) return 1;
        if (lead < 0.55) return 2;
        if (lead < 1.10) return 4;
        return 8;
    };
    auto flushPending = [&](bool force) {
        const int n = static_cast<int>(pending.size() / nVq_);
        if (n == 0 || (!force && n < decodeBudget())) return;
        emit(runCodecFrames(pending, n), clock, onChunk);
        pending.clear();
    };

    for (int step = 0; step < maxFrames; ++step) {
        if (cancelFlag.load()) return false;

        // Local transformer: sample one frame of nVq audio codes from the global hidden state.
        int32_t shouldContinue = 0;
        {
            assistantU[0] = uniform(rng_);
            for (auto& u : audioU) u = uniform(rng_);
            const int64_t hShape[] = {1, static_cast<int64_t>(hidden.size())};
            const int64_t mShape[] = {1, nVq_, codebookSize_};
            const int64_t aShape[] = {1};
            const int64_t uShape[] = {1, nVq_};
            auto inputs = nullValues(localInNames_.size());
            for (size_t i = 0; i < localInNames_.size(); ++i) {
                const auto& n = localInNames_[i];
                if (n == "global_hidden")
                    inputs[i] = Ort::Value::CreateTensor<float>(memInfo_, hidden.data(), hidden.size(), hShape, 2);
                else if (n == "repetition_seen_mask")
                    inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, seenMask.data(), seenMask.size(), mShape, 3);
                else if (n == "assistant_random_u")
                    inputs[i] = Ort::Value::CreateTensor<float>(memInfo_, assistantU.data(), 1, aShape, 1);
                else if (n == "audio_random_u")
                    inputs[i] = Ort::Value::CreateTensor<float>(memInfo_, audioU.data(), audioU.size(), uShape, 2);
                else
                    throw std::runtime_error("Unexpected local frame input: " + n);
            }
            auto out = localFrameSession_->Run(Ort::RunOptions{nullptr}, localIn.data(), inputs.data(),
                                               inputs.size(), localOut.data(), localOut.size());
            for (size_t i = 0; i < localOutNames_.size(); ++i) {
                if (localOutNames_[i] == "should_continue") {
                    shouldContinue = out[i].GetTensorData<int32_t>()[0];
                } else if (localOutNames_[i] == "frame_token_ids") {
                    const int32_t* f = out[i].GetTensorData<int32_t>();
                    stepRow[0] = audioAssistantSlotTokenId_;
                    for (int q = 0; q < nVq_; ++q) stepRow[q + 1] = f[q];
                }
            }
        }
        if (!shouldContinue) break;

        for (int q = 0; q < nVq_; ++q) {
            const int32_t tok = stepRow[q + 1];
            if (tok >= 0 && tok < codebookSize_) seenMask[static_cast<size_t>(q) * codebookSize_ + tok] = 1;
        }
        pending.insert(pending.end(), stepRow.begin() + 1, stepRow.end());
        flushPending(false);
        if (cancelFlag.load()) return false;

        // Global transformer: append the frame and advance the KV cache.
        {
            const int64_t rowShape[] = {1, 1, rowWidth_};
            const int64_t lenShape[] = {1};
            auto inputs = nullValues(decodeInNames_.size());
            for (size_t i = 0; i < decodeInNames_.size(); ++i) {
                if (decodeInNames_[i] == "input_ids")
                    inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, stepRow.data(), stepRow.size(), rowShape, 3);
                else if (decodeInNames_[i] == "past_valid_lengths")
                    inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, &pastValidLength, 1, lenShape, 1);
            }
            for (size_t k = 0; k < kvDecodeInIdx_.size(); ++k) inputs[kvDecodeInIdx_[k]] = std::move(kv[k]);

            auto out = decodeSession_->Run(Ort::RunOptions{nullptr}, decodeIn.data(), inputs.data(),
                                           inputs.size(), decodeOut.data(), decodeOut.size());
            hidden = lastHidden(out[decodeHiddenOutIdx_]);
            for (size_t k = 0; k < kvDecodeInIdx_.size(); ++k) kv[k] = std::move(out[kvDecodeOutIdx_[k]]);
            ++pastValidLength;
        }
    }

    if (cancelFlag.load()) return false;
    flushPending(true);
    return !cancelFlag.load();
}

// ---------------------------------------------------------------------------
// Streaming codec (CodecStreamingDecodeSession)
// ---------------------------------------------------------------------------

void MossTTSImpl::resetCodecState() {
    Ort::AllocatorWithDefaultOptions alloc;
    codecState_.clear();
    codecState_.reserve(codecStateSpecs_.size());
    for (const auto& s : codecStateSpecs_) {
        size_t count = 1;
        for (int64_t d : s.shape) count *= static_cast<size_t>(d);
        if (s.isFloat) {
            auto v = Ort::Value::CreateTensor<float>(alloc, s.shape.data(), s.shape.size());
            std::fill_n(v.GetTensorMutableData<float>(), count, 0.0f);
            codecState_.push_back(std::move(v));
        } else {
            auto v = Ort::Value::CreateTensor<int32_t>(alloc, s.shape.data(), s.shape.size());
            std::fill_n(v.GetTensorMutableData<int32_t>(), count, s.fillInt);
            codecState_.push_back(std::move(v));
        }
    }
}

std::vector<float> MossTTSImpl::runCodecFrames(const std::vector<int32_t>& frames, int nFrames) {
    std::vector<int32_t> codes(frames);
    int32_t codeLength = nFrames;
    const int64_t codesShape[] = {1, nFrames, nVq_};
    const int64_t lenShape[]   = {1};

    auto inputs = nullValues(codecInNames_.size());
    for (size_t i = 0; i < codecInNames_.size(); ++i) {
        if (codecInNames_[i] == "audio_codes")
            inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, codes.data(), codes.size(), codesShape, 3);
        else if (codecInNames_[i] == "audio_code_lengths")
            inputs[i] = Ort::Value::CreateTensor<int32_t>(memInfo_, &codeLength, 1, lenShape, 1);
    }
    for (size_t s = 0; s < codecStateSpecs_.size(); ++s)
        inputs[codecStateSpecs_[s].inIdx] = std::move(codecState_[s]);

    const auto inNames  = cstrs(codecInNames_);
    const auto outNames = cstrs(codecOutNames_);
    auto out = codecStepSession_->Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
                                      outNames.data(), outNames.size());
    for (size_t s = 0; s < codecStateSpecs_.size(); ++s)
        codecState_[s] = std::move(out[codecStateSpecs_[s].outIdx]);

    size_t audioIdx = 0, lenIdx = 0;
    if (!indexOf(codecOutNames_, "audio", audioIdx) || !indexOf(codecOutNames_, "audio_lengths", lenIdx))
        throw std::runtime_error("Codec graph lacks audio/audio_lengths outputs");

    // audio: [1, channels, samples] channel-major → mono average over the valid length.
    const auto shape      = out[audioIdx].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t chans   = shape.size() == 3 ? shape[1] : 1;
    const int64_t samples = shape.back();
    const int64_t valid   = std::min<int64_t>(samples, out[lenIdx].GetTensorData<int32_t>()[0]);
    std::vector<float> mono(static_cast<size_t>(std::max<int64_t>(0, valid)), 0.0f);
    const float* a = out[audioIdx].GetTensorData<float>();
    for (int64_t c = 0; c < chans; ++c)
        for (int64_t i = 0; i < valid; ++i) mono[i] += a[c * samples + i];
    if (chans > 1)
        for (auto& x : mono) x /= static_cast<float>(chans);
    return mono;
}
