#pragma once

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <VASDK/profiling/CTrack.h>
#include <onnxruntime_cxx_api.h>
#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif
#include <spdlog/spdlog.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <memory>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <chrono>

class ParakeetInferenceProviderImpl {
public:
    struct InferResult {
        std::string text;
        std::vector<va::audio::TranscriptionResult::LanguageTag> languages; // detected languages in order of first appearance
        float confidence = 0.0f;
        std::vector<va::audio::TranscriptionResult::Word> words;
    };
    bool initialize(const std::string& modelDir, bool useCuda) {
        _modelDir = modelDir;
        if (!loadVocab(modelDir + "/vocab.txt")) return false;

        _env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "Parakeet");

        // Load preprocessor (nemo128.onnx)
        // useNnapi=true: runs once per utterance - NNAPI+FP16 GPU/DSP acceleration worthwhile.
        std::string prepPath = modelDir + "/nemo128.onnx";
        if (!loadSession(prepPath, _preprocessor, _prepIO, false)) return false;

        // Load encoder (prefer int8)
        // useNnapi=true: runs once per utterance - largest model, biggest gain from NNAPI+FP16.
        std::string encPath = findModel(modelDir, "encoder-model");
        if (encPath.empty()) { spdlog::error("[Parakeet] No encoder model found"); return false; }
        if (!loadSession(encPath, _encoder, _encIO, useCuda)) return false;

        // Load decoder-joint (prefer int8)
        // useNnapi=false: called ~once per encoder frame in the TDT loop - NNAPI dispatch
        // overhead (~1-5 ms/call) would dominate for short utterances. CPU is faster here.
        std::string decPath = findModel(modelDir, "decoder_joint-model");
        if (decPath.empty()) { spdlog::error("[Parakeet] No decoder-joint model found"); return false; }
        if (!loadSession(decPath, _decoderJoint, _decIO, useCuda)) return false;

        // Query decoder LSTM state dimensions from input_states_1 shape [L, 1, H]
        for (size_t i = 0; i < _decIO.in.size(); i++) {
            if (_decIO.in[i] == "input_states_1") {
                auto shape = _decoderJoint->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
                if (shape.size() >= 3 && shape[0] > 0 && shape[2] > 0) {
                    _stateLayers = static_cast<int>(shape[0]);
                    _stateHidden = static_cast<int>(shape[2]);
                }
                break;
            }
        }
        if (_stateLayers == 0 || _stateHidden == 0) {
            spdlog::error("[Parakeet] Could not determine decoder state dimensions");
            return false;
        }

        // Resolve decoder input indices by name
        for (size_t i = 0; i < _decIO.in.size(); i++) {
            const auto& name = _decIO.in[i];
            if (name.find("encoder") != std::string::npos) _decIdxEnc = static_cast<int>(i);
            else if (name == "targets") _decIdxTarg = static_cast<int>(i);
            else if (name.find("target_length") != std::string::npos) _decIdxTlen = static_cast<int>(i);
            else if (name.find("input_states_1") != std::string::npos) _decIdxS1 = static_cast<int>(i);
            else if (name.find("input_states_2") != std::string::npos) _decIdxS2 = static_cast<int>(i);
        }

        // Resolve decoder output indices by name
        for (size_t i = 0; i < _decIO.out.size(); i++) {
            const auto& name = _decIO.out[i];
            if (name == "outputs") _decOutLogits = static_cast<int>(i);
            else if (name.find("output_states_1") != std::string::npos) _decOutS1 = static_cast<int>(i);
            else if (name.find("output_states_2") != std::string::npos) _decOutS2 = static_cast<int>(i);
        }

        spdlog::info("[Parakeet] Decoder I/O mapping: in[enc={},targ={},tlen={},s1={},s2={}] out[logits={},s1={},s2={}]",
                     _decIdxEnc, _decIdxTarg, _decIdxTlen, _decIdxS1, _decIdxS2,
                     _decOutLogits, _decOutS1, _decOutS2);

        spdlog::info("[Parakeet] Initialized: vocab={}, blank={}, state=[{},1,{}]",
                     _vocabSize, _blankIdx, _stateLayers, _stateHidden);
        if (!initDecoderBinding()) return false;
        return true;
    }

    // isPartial=true enables incremental mel+encoder caching: only new audio is preprocessed
    // and only new mel frames are encoded; results are appended to the cached prefix.
    // isPartial=false (final) always runs the full pipeline and resets the cache.
    InferResult infer(const float* audio, size_t numSamples, int sampleRate = 16000,
                      bool isPartial = false) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_preprocessor || !_encoder || !_decoderJoint || numSamples == 0) return {};

        // Finals always run fresh; clear stale incremental state before doing anything.
        if (!isPartial) clearIncrementalCache();

        // 1. Mel preprocessing - incremental or full
        int64_t featLen;
        if (isPartial && _incrFeatLen > 0 && _incrHop > 0 && numSamples > _incrWavLen) {
            featLen = preprocessIncremental(audio, numSamples);
        } else {
            featLen = preprocess(audio, numSamples);
            // Derive mel hop once so preprocessIncremental() can compute slice offsets.
            if (isPartial && _incrHop == 0 && featLen > 0)
                _incrHop = static_cast<int>(numSamples / featLen);
        }
        if (featLen < 0) return {};

        // Cache stable mel frames for the next partial (drop kMelBoundaryFrames from the
        // tail - those frames' windows may shift when more audio arrives).
        if (isPartial) {
            int64_t stableLen = std::max(int64_t{0}, featLen - kMelBoundaryFrames);
            _incrFeatCache.resize(static_cast<size_t>(stableLen) * 128);
            std::memcpy(_incrFeatCache.data(), _featuresBuffer.data(),
                        static_cast<size_t>(stableLen) * 128 * sizeof(float));
            _incrFeatLen = stableLen;
            _incrWavLen  = numSamples;
        }

        // 2. Encoding - incremental (encode only new mel slice) or full
        int Tp = 0, D = 0;
        auto t0Enc = std::chrono::steady_clock::now();

        const bool canIncrEnc = isPartial && _incrEncLen > 0
                                && _incrMelLen > 0 && featLen > _incrMelLen;
        if (canIncrEnc) {
            D = _encoderHiddenDim;

            // Restore cached transposed encoder frames into _encTransposeBuffer.
            _encTransposeBuffer.resize(static_cast<size_t>(_incrEncLen) * D);
            std::memcpy(_encTransposeBuffer.data(), _incrEncCache.data(),
                        static_cast<size_t>(_incrEncLen) * D * sizeof(float));

            // Encode mel slice [_incrMelLen .. featLen] from _featuresBuffer.
            try {
                auto& mem = _memInfo;
                constexpr int melBins = 128;
                int sliceFrames = static_cast<int>(featLen - _incrMelLen);
                std::array<int64_t, 3> sliceShape = {1, melBins, sliceFrames};
                auto sliceFeatT = Ort::Value::CreateTensor<float>(
                    mem,
                    _featuresBuffer.data() + static_cast<size_t>(_incrMelLen) * melBins,
                    static_cast<size_t>(sliceFrames) * melBins,
                    sliceShape.data(), 3);

                int64_t sliceLen = sliceFrames;
                std::array<int64_t, 1> sliceLenShape = {1};
                auto sliceLenT = Ort::Value::CreateTensor<int64_t>(
                    mem, &sliceLen, 1, sliceLenShape.data(), 1);

                std::vector<Ort::Value> sliceIn;
                sliceIn.push_back(std::move(sliceFeatT));
                sliceIn.push_back(std::move(sliceLenT));
                auto out = _encoder->Run(Ort::RunOptions{nullptr},
                    _encIO.inP.data(), sliceIn.data(), sliceIn.size(),
                    _encIO.outP.data(), _encIO.outP.size());

                auto& encTensor = out[0];
                auto  encShape  = encTensor.GetTensorTypeAndShapeInfo().GetShape();
                int64_t newEncLen = *out[1].GetTensorData<int64_t>();
                D = static_cast<int>(encShape[1]);

                // Tiled transpose of the new slice and APPEND to _encTransposeBuffer.
                int64_t totalTp = _incrEncLen + newEncLen;
                _encTransposeBuffer.resize(static_cast<size_t>(totalTp) * D);
                const float* encData = encTensor.GetTensorData<float>();
                float* dst = _encTransposeBuffer.data() + static_cast<size_t>(_incrEncLen) * D;
                int newTp = static_cast<int>(newEncLen);
                constexpr int kTile = 32;
                for (int t = 0; t < newTp; t += kTile)
                    for (int d = 0; d < D; d += kTile)
                        for (int tt = t, ttEnd = std::min(t + kTile, newTp); tt < ttEnd; tt++)
                            for (int dd = d, ddEnd = std::min(d + kTile, D); dd < ddEnd; dd++)
                                dst[tt * D + dd] = encData[dd * newTp + tt];

                Tp = static_cast<int>(totalTp);
            } catch (const Ort::Exception& e) {
                spdlog::error("[Parakeet] Incremental encode error: {}", e.what());
                return {};
            }
        } else {
            // Full encode + tiled transpose [1, D, T'] → [T', D]
            auto [encLen, hiddenDim] = encode(featLen);
            if (encLen == 0) return {};
            Tp = static_cast<int>(encLen);
            D  = hiddenDim;
            _encTransposeBuffer.resize(static_cast<size_t>(Tp) * D);
            constexpr int kTile = 32;
            for (int t = 0; t < Tp; t += kTile)
                for (int d = 0; d < D; d += kTile)
                    for (int tt = t, ttEnd = std::min(t + kTile, Tp); tt < ttEnd; tt++)
                        for (int dd = d, ddEnd = std::min(d + kTile, D); dd < ddEnd; dd++)
                            _encTransposeBuffer[tt * D + dd] = _encOutBuffer[dd * Tp + tt];
        }

        if (_timingObserver) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0Enc).count();
            _timingObserver->onTiming("encoder", ms);
        }
        if (Tp == 0) return {};

        // Cache combined (cached + new) transposed encoder output for the next partial.
        if (isPartial) {
            _incrEncCache.resize(static_cast<size_t>(Tp) * D);
            std::memcpy(_incrEncCache.data(), _encTransposeBuffer.data(),
                        static_cast<size_t>(Tp) * D * sizeof(float));
            _incrEncLen = Tp;
            _incrMelLen = featLen;
        }

        // 3. TDT greedy decode (returns per-token frame indices and confidences)
        auto t0Dec = std::chrono::steady_clock::now();
        auto decResult = tdtDecode(_encTransposeBuffer.data(), Tp, D);
        if (_timingObserver) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0Dec).count();
            _timingObserver->onTiming("decoder", ms);
        }

        // 4. Compute ms-per-encoder-frame using NeMo Parakeet model constants:
        //    mel hop size = 10ms, FastConformer encoder stride = 8x → 80ms per encoder frame.
        //    The ratio-based formula (numSamples / (sampleRate * Tp)) is incorrect because
        //    ceil()-based subsampling in the encoder makes Tp * 80ms > total audio duration
        //    for short utterances, producing a systematically wrong per-frame duration.
        constexpr double kMsPerEncoderFrame = 10.0 * 8.0; // 10ms mel hop × 8 encoder stride
        double msPerFrame = kMsPerEncoderFrame;

        // 5. Partition decoded tokens:
        //    - language tokens  → collect with confidence; track active language for word tagging
        //    - other specials   → silently drop (control tokens like <|nospeech|>)
        //    - normal tokens    → build transcription text and word timings
        using LanguageTag = va::audio::TranscriptionResult::LanguageTag;
        std::vector<LanguageTag> detectedLanguages;          // unique languages, first-appearance order
        std::unordered_map<std::string, size_t> langIndex;   // code -> index in detectedLanguages
        std::vector<std::pair<int, LanguageTag>> langEmissions; // (frameIndex, tag) for per-word tagging
        std::vector<int> textTokenIds;
        std::vector<DecodeResult::TokenInfo> textTokenInfos;

        for (const auto& ti : decResult.tokenInfos) {
            auto langIt = _languageTokenIds.find(ti.token);
            if (langIt != _languageTokenIds.end()) {
                spdlog::debug("[Parakeet] Language token: <|{}|> (id={}) conf={:.3f} frame={}",
                              langIt->second, ti.token, ti.tokenConfidence, ti.frameIndex);
                LanguageTag tag{langIt->second, ti.tokenConfidence};
                langEmissions.push_back({ti.frameIndex, tag});
                // Track unique languages; if seen again, keep the highest confidence.
                auto [mapIt, inserted] = langIndex.emplace(langIt->second, detectedLanguages.size());
                if (inserted) {
                    detectedLanguages.push_back(tag);
                } else if (ti.tokenConfidence > detectedLanguages[mapIt->second].confidence) {
                    detectedLanguages[mapIt->second].confidence = ti.tokenConfidence;
                }
            } else if (_specialTokenIds.count(ti.token) == 0) {
                textTokenIds.push_back(ti.token);
                textTokenInfos.push_back(ti);
            } else {
                // Special/control token (non-language) - log at trace level for diagnostics
                auto vocabIt = _vocab.find(ti.token);
                spdlog::trace("[Parakeet] Special token dropped: {} (id={}) frame={}",
                              vocabIt != _vocab.end() ? vocabIt->second : "?", ti.token, ti.frameIndex);
            }
        }

        return {decodeTokens(textTokenIds), detectedLanguages, decResult.confidence, buildWords(textTokenInfos, msPerFrame, langEmissions)};
    }

    void setTimingObserver(va::metrics::TimingObserverInterface* observer) {
        _timingObserver = observer;
    }

    void release() {
        _decIoBinding.reset();   // must precede _decoderJoint - binding depends on session lifetime
        _decoderJoint.reset();
        _encoder.reset();
        _preprocessor.reset();
        _env.reset();
    }

private:
    // --- I/O name storage ---
    struct ModelIO {
        std::vector<std::string> in, out;
        std::vector<const char*> inP, outP;
    };

    static ModelIO queryIO(Ort::Session& session) {
        ModelIO io;
        Ort::AllocatorWithDefaultOptions alloc;
        io.in.reserve(session.GetInputCount());
        for (size_t i = 0; i < session.GetInputCount(); i++)
            io.in.push_back(session.GetInputNameAllocated(i, alloc).get());
        io.out.reserve(session.GetOutputCount());
        for (size_t i = 0; i < session.GetOutputCount(); i++)
            io.out.push_back(session.GetOutputNameAllocated(i, alloc).get());
        for (auto& n : io.in) io.inP.push_back(n.c_str());
        for (auto& n : io.out) io.outP.push_back(n.c_str());
        return io;
    }

    // --- File helpers ---
    static std::string findModel(const std::string& dir, const std::string& base) {
        std::string int8 = dir + "/" + base + ".int8.onnx";
        std::string fp32 = dir + "/" + base + ".onnx";
        if (std::filesystem::exists(int8)) return int8;
        if (std::filesystem::exists(fp32)) return fp32;
        return "";
    }

    // --- Vocab ---
    bool loadVocab(const std::string& path) {
        // Open in binary mode so Windows text-mode doesn't mangle multi-byte
        // UTF-8 sequences and we can handle line endings ourselves.
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            spdlog::error("[Parakeet] Cannot open vocab: {}", path);
            return false;
        }

        // Strip UTF-8 BOM (EF BB BF) if present - common when files are
        // saved on Windows with editors that emit a BOM.
        {
            char bom[3] = {};
            if (f.read(bom, 3)) {
                bool hasBom = static_cast<uint8_t>(bom[0]) == 0xEF &&
                              static_cast<uint8_t>(bom[1]) == 0xBB &&
                              static_cast<uint8_t>(bom[2]) == 0xBF;
                if (!hasBom) f.seekg(0);
            }
        }

        _vocab.clear();
        _blankIdx = -1;
        std::string line;
        while (std::getline(f, line)) {
            // Strip trailing \r from Windows-style \r\n line endings.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            // Format: "token_string id" (space-separated, split on last space)
            auto pos = line.rfind(' ');
            if (pos == std::string::npos) continue;
            std::string token = line.substr(0, pos);
            int id = std::stoi(line.substr(pos + 1));

            // Replace SentencePiece ▁ (U+2581, UTF-8: E2 96 81) with space
            std::string cleaned;
            for (size_t i = 0; i < token.size();) {
                if (i + 2 < token.size() &&
                    static_cast<uint8_t>(token[i]) == 0xE2 &&
                    static_cast<uint8_t>(token[i + 1]) == 0x96 &&
                    static_cast<uint8_t>(token[i + 2]) == 0x81) {
                    cleaned += ' ';
                    i += 3;
                } else {
                    cleaned += token[i++];
                }
            }
            _vocab[id] = cleaned;
            if (token == "<blk>") _blankIdx = id;
            if (token == "<|predict_lang|>") _predictLangIdx = id;

            // Mark special/control tokens (original token is wrapped in < >) -
            // these should never appear in transcribed text.
            if (!token.empty() && token.front() == '<') {
                _specialTokenIds.insert(id);
                // Identify language tokens: exactly <|xx|> where xx is two lowercase letters.
                // e.g. <|en|> (size=6): '<','|','e','n','|','>'
                if (token.size() == 6 && token[1] == '|' && token[4] == '|' && token[5] == '>') {
                    const char c0 = token[2], c1 = token[3];
                    if (c0 >= 'a' && c0 <= 'z' && c1 >= 'a' && c1 <= 'z')
                        _languageTokenIds[id] = std::string({c0, c1});
                }
            }
        }
        _vocabSize = static_cast<int>(_vocab.size());

        if (_blankIdx < 0) {
            // Default: blank is last token
            _blankIdx = _vocabSize - 1;
            spdlog::warn("[Parakeet] <blk> not found in vocab, using last ID={}", _blankIdx);
        }
        spdlog::info("[Parakeet] Vocab: {} tokens, blank={}, predict_lang={}",
                     _vocabSize, _blankIdx, _predictLangIdx);
        return _vocabSize > 0;
    }

    // --- Session loading ---
    // useNnapi controls whether NNAPI is registered on Android.
    // Set false for the decoder-joint which runs in the TDT per-frame loop: NNAPI dispatch
    // overhead (~1-5 ms/call) exceeds the inference time for this small network and
    // results in higher latency than plain CPU execution.
    bool loadSession(const std::string& path, std::unique_ptr<Ort::Session>& session, ModelIO& io,
                     bool useCuda) {
        try {
            Ort::SessionOptions opts;
            bool useNnapi = false;
#ifdef VASDK_USE_NNAPI
            useNnapi = true;
#endif            
            // Decoder-joint is a small network called once per encoder frame in a tight loop;
            // reducing threads avoids scheduling overhead exceeding the computation time.
            const int intraThreads = useNnapi ? 4 : 2;
            opts.SetIntraOpNumThreads(intraThreads);
            opts.SetInterOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            opts.SetExecutionMode(ExecutionMode::ORT_PARALLEL);
            if (useCuda) {
                OrtCUDAProviderOptions cudaOpts{};
                opts.AppendExecutionProvider_CUDA(cudaOpts);
            }
#ifdef VASDK_USE_NNAPI
            if (useNnapi) {
                // NNAPI_FLAG_USE_FP16: run FP32 ops as FP16 on the mobile GPU/DSP.
                // Do NOT add NNAPI_FLAG_CPU_DISABLED: if any op is unsupported ONNX
                // Runtime falls back to CPU automatically, avoiding a hard failure.
                OrtSessionOptionsAppendExecutionProvider_Nnapi(opts, NNAPI_FLAG_USE_FP16);
            }
#endif
            std::filesystem::path fspath(path);
            session = std::make_unique<Ort::Session>(*_env, fspath.c_str(), opts);
            io = queryIO(*session);

            spdlog::info("[Parakeet] Loaded {} ({} in, {} out)",
                         fspath.filename().string(), io.in.size(), io.out.size());
            return true;
        } catch (const Ort::Exception& e) {
            spdlog::error("[Parakeet] Failed to load {}: {}", path, e.what());
            return false;
        }
    }

    // --- IoBinding setup for decoder-joint ---
    // Called once from initialize() after all session I/O indices are resolved.
    // Binds pre-allocated CPU buffers so tdtDecode() can call Run(opts, binding) each step
    // without creating or destroying Ort::Value tensors in the hot loop.
    bool initDecoderBinding() {
        // Query encoder hidden dim D from output shape [1, D, T'] (T' is dynamic)
        auto encOutShape = _encoder->GetOutputTypeInfo(0)
                               .GetTensorTypeAndShapeInfo().GetShape();
        _encoderHiddenDim = static_cast<int>(encOutShape[1]);

        // Query total logit output width (vocab + num_durations) from decoder output shape
        auto decOutShape = _decoderJoint->GetOutputTypeInfo(_decOutLogits)
                               .GetTensorTypeAndShapeInfo().GetShape();
        _decTotalOutDim = static_cast<int>(decOutShape.back());

        if (_encoderHiddenDim <= 0 || _decTotalOutDim <= 0) {
            spdlog::error("[Parakeet] Cannot resolve enc/dec dims for IoBinding (enc={}, dec={})",
                          _encoderHiddenDim, _decTotalOutDim);
            return false;
        }

        int stateSize = _stateLayers * _stateHidden;
        _decEncFrameBuf.resize(_encoderHiddenDim);
        _decTargetBuf.resize(1, 0);
        _decS1InBuf.resize(stateSize, 0.0f);
        _decS2InBuf.resize(stateSize, 0.0f);
        _decS1OutBuf.resize(stateSize);
        _decS2OutBuf.resize(stateSize);
        _decLogitsBuf.resize(_decTotalOutDim);

        auto& mem = _memInfo;
        std::array<int64_t, 3> encFShape = {1, _encoderHiddenDim, 1};
        std::array<int64_t, 2> targShape  = {1, 1};
        std::array<int64_t, 1> tlenShape  = {1};
        std::array<int64_t, 3> stShape    = {_stateLayers, 1, _stateHidden};
        std::array<int64_t, 4> logShape   = {1, 1, 1, _decTotalOutDim};

        // Wrap pre-allocated buffers - ORT increments an internal ref-count during BindInput/BindOutput,
        // so these local Ort::Value objects can safely go out of scope after binding.
        Ort::Value encV  = Ort::Value::CreateTensor<float>  (mem, _decEncFrameBuf.data(), _encoderHiddenDim, encFShape.data(), 3);
        Ort::Value targV = Ort::Value::CreateTensor<int32_t>(mem, _decTargetBuf.data(),   1,                 targShape.data(), 2);
        Ort::Value tlenV = Ort::Value::CreateTensor<int32_t>(mem, &_decTLenBuf,           1,                 tlenShape.data(), 1);
        Ort::Value s1V   = Ort::Value::CreateTensor<float>  (mem, _decS1InBuf.data(),     stateSize,         stShape.data(),   3);
        Ort::Value s2V   = Ort::Value::CreateTensor<float>  (mem, _decS2InBuf.data(),     stateSize,         stShape.data(),   3);
        Ort::Value logV  = Ort::Value::CreateTensor<float>  (mem, _decLogitsBuf.data(),   _decTotalOutDim,   logShape.data(),  4);
        Ort::Value os1V  = Ort::Value::CreateTensor<float>  (mem, _decS1OutBuf.data(),    stateSize,         stShape.data(),   3);
        Ort::Value os2V  = Ort::Value::CreateTensor<float>  (mem, _decS2OutBuf.data(),    stateSize,         stShape.data(),   3);

        _decIoBinding = std::make_unique<Ort::IoBinding>(*_decoderJoint);
        _decIoBinding->BindInput (_decIO.in[_decIdxEnc].c_str(),    encV);
        _decIoBinding->BindInput (_decIO.in[_decIdxTarg].c_str(),   targV);
        _decIoBinding->BindInput (_decIO.in[_decIdxTlen].c_str(),   tlenV);
        _decIoBinding->BindInput (_decIO.in[_decIdxS1].c_str(),     s1V);
        _decIoBinding->BindInput (_decIO.in[_decIdxS2].c_str(),     s2V);
        _decIoBinding->BindOutput(_decIO.out[_decOutLogits].c_str(), logV);
        _decIoBinding->BindOutput(_decIO.out[_decOutS1].c_str(),     os1V);
        _decIoBinding->BindOutput(_decIO.out[_decOutS2].c_str(),     os2V);

        spdlog::info("[Parakeet] IoBinding initialized: enc_dim={}, total_out={}",
                     _encoderHiddenDim, _decTotalOutDim);
        return true;
    }

    // --- Incremental inference helpers ---

    void clearIncrementalCache() {
        _incrFeatLen = 0;
        _incrWavLen  = 0;
        _incrHop     = 0;
        _incrEncLen  = 0;
        _incrMelLen  = 0;
        _incrFeatCache.clear();
        _incrEncCache.clear();
    }

    // Runs the preprocessor on audio[startSample..numSamples], then stitches with
    // the stable cached frames so _featuresBuffer contains the full mel sequence.
    // startSample = _incrFeatLen * _incrHop (first sample of the first boundary frame).
    // The first kMelBoundaryFrames of the new output are discarded because their mel
    // windows extend before startSample and are zero-padded there.
    int64_t preprocessIncremental(const float* audio, size_t numSamples) {
        size_t startSample = static_cast<size_t>(_incrFeatLen) * static_cast<size_t>(_incrHop);
        if (startSample >= numSamples)
            return preprocess(audio, numSamples); // audio didn't grow past the cache - full fallback

        int64_t newFeatLen = preprocess(audio + startSample, numSamples - startSample);
        if (newFeatLen < 0) return -1;

        int64_t usableNew = newFeatLen - kMelBoundaryFrames;
        if (usableNew <= 0) {
            // New audio too short to yield clean frames: restore cache as _featuresBuffer.
            _featuresBuffer.resize(static_cast<size_t>(_incrFeatLen) * 128);
            std::memcpy(_featuresBuffer.data(), _incrFeatCache.data(),
                        static_cast<size_t>(_incrFeatLen) * 128 * sizeof(float));
            return _incrFeatLen;
        }

        // Save usable new frames from _featuresBuffer (written by preprocess()) before we
        // overwrite it with the stitched result.
        size_t usableOffset = static_cast<size_t>(kMelBoundaryFrames) * 128;
        size_t usableBytes  = static_cast<size_t>(usableNew) * 128;
        _incrMelTmpBuf.resize(usableBytes);
        std::memcpy(_incrMelTmpBuf.data(), _featuresBuffer.data() + usableOffset,
                    usableBytes * sizeof(float));

        // Rebuild _featuresBuffer = [stable cached frames] + [new usable frames]
        int64_t totalFeatLen = _incrFeatLen + usableNew;
        _featuresBuffer.resize(static_cast<size_t>(totalFeatLen) * 128);
        std::memcpy(_featuresBuffer.data(),
                    _incrFeatCache.data(),
                    static_cast<size_t>(_incrFeatLen) * 128 * sizeof(float));
        std::memcpy(_featuresBuffer.data() + static_cast<size_t>(_incrFeatLen) * 128,
                    _incrMelTmpBuf.data(),
                    usableBytes * sizeof(float));

        return totalFeatLen;
    }

    // --- Preprocessing: nemo128.onnx ---
    // Input: waveforms [1, N] float32, waveforms_lens [1] int64
    // Output: writes features into _featuresBuffer [1, 128, T], returns featLen or -1 on error
    int64_t preprocess(const float* audio, size_t numSamples) {
        VASDK_CTRACK_NAME("Parakeet::preprocess");
        try {
            auto& mem = _memInfo;

            std::array<int64_t, 2> wavShape = {1, static_cast<int64_t>(numSamples)};
            auto wavT = Ort::Value::CreateTensor<float>(
                mem, const_cast<float*>(audio), numSamples, wavShape.data(), 2);

            int64_t wavLen = static_cast<int64_t>(numSamples);
            std::array<int64_t, 1> lenShape = {1};
            auto lenT = Ort::Value::CreateTensor<int64_t>(mem, &wavLen, 1, lenShape.data(), 1);

            std::vector<Ort::Value> inputs;
            inputs.push_back(std::move(wavT));
            inputs.push_back(std::move(lenT));

            auto out = _preprocessor->Run(
                Ort::RunOptions{nullptr},
                _prepIO.inP.data(), inputs.data(), inputs.size(),
                _prepIO.outP.data(), _prepIO.outP.size());

            // Copy features into reusable member buffer (avoids per-inference allocation)
            auto& featTensor = out[0];
            auto featShape = featTensor.GetTensorTypeAndShapeInfo().GetShape();
            size_t featSize = 1;
            for (auto s : featShape) featSize *= s;
            const float* featData = featTensor.GetTensorData<float>();
            _featuresBuffer.resize(featSize);
            std::memcpy(_featuresBuffer.data(), featData, featSize * sizeof(float));

            int64_t featLen = *out[1].GetTensorData<int64_t>();

            spdlog::debug("[Parakeet] Preprocessed: {} samples -> [{},{},{}] features (len={})",
                          numSamples, featShape[0], featShape[1], featShape[2], featLen);
            return featLen;
        } catch (const Ort::Exception& e) {
            spdlog::error("[Parakeet] Preprocess error: {}", e.what());
            return -1;
        }
    }

    // --- Encoding: encoder-model.onnx ---
    // Input: audio_signal [1, 128, T] float32, length [1] int64
    // Output: writes encoder output into _encOutBuffer [1, D, T'], returns {encLen, hiddenDim} or {0,0} on error
    struct EncodeResult {
        int64_t length;
        int hiddenDim;
    };

    EncodeResult encode(int64_t featLen) {
        VASDK_CTRACK_NAME("Parakeet::encode");
        try {
            auto& mem = _memInfo;

            // Determine feature tensor shape from member buffer size
            int melBins = 128;
            int timeFrames = static_cast<int>(_featuresBuffer.size()) / melBins;
            std::array<int64_t, 3> featShape = {1, melBins, timeFrames};
            auto featT = Ort::Value::CreateTensor<float>(
                mem, _featuresBuffer.data(), _featuresBuffer.size(), featShape.data(), 3);

            std::array<int64_t, 1> lenShape = {1};
            auto lenT = Ort::Value::CreateTensor<int64_t>(mem, &featLen, 1, lenShape.data(), 1);

            std::vector<Ort::Value> inputs;
            inputs.push_back(std::move(featT));
            inputs.push_back(std::move(lenT));

            auto out = _encoder->Run(
                Ort::RunOptions{nullptr},
                _encIO.inP.data(), inputs.data(), inputs.size(),
                _encIO.outP.data(), _encIO.outP.size());

            auto& encTensor = out[0];
            auto encShape = encTensor.GetTensorTypeAndShapeInfo().GetShape();
            size_t encSize = 1;
            for (auto s : encShape) encSize *= s;
            const float* encData = encTensor.GetTensorData<float>();
            // Copy into reusable member buffer (avoids per-inference allocation)
            _encOutBuffer.resize(encSize);
            std::memcpy(_encOutBuffer.data(), encData, encSize * sizeof(float));

            int64_t encLen = *out[1].GetTensorData<int64_t>();
            int D = static_cast<int>(encShape[1]); // hidden dim

            spdlog::debug("[Parakeet] Encoded: [{},{},{}] -> len={}, D={}",
                          encShape[0], encShape[1], encShape[2], encLen, D);
            return {encLen, D};
        } catch (const Ort::Exception& e) {
            spdlog::error("[Parakeet] Encode error: {}", e.what());
            return {0, 0};
        }
    }

    // --- TDT Greedy Decoding ---
    struct DecodeResult {
        struct TokenInfo {
            int token;
            int frameIndex;        // encoder frame at which this token was emitted
            float tokenConfidence; // per-token sigmoid(best - secondBest) confidence
        };
        std::vector<TokenInfo> tokenInfos;
        float confidence = 0.0f;
    };

    DecodeResult tdtDecode(const float* encT, int encLen, int D) {
        VASDK_CTRACK_NAME("Parakeet::decode");
        int stateSize = _stateLayers * _stateHidden;
        // Reset input state buffers to zero at the start of each utterance.
        std::fill(_decS1InBuf.begin(), _decS1InBuf.end(), 0.0f);
        std::fill(_decS2InBuf.begin(), _decS2InBuf.end(), 0.0f);

        Ort::RunOptions runOpts{nullptr};
        // Split: first _vocabSize are token logits, rest are duration logits
        int numDurations = _decTotalOutDim - _vocabSize;

        std::vector<DecodeResult::TokenInfo> tokenInfos;
        double sumLogProb = 0.0;
        int numEmitted = 0;
        int t = 0;
        int emitted = 0;

        while (t < encLen) {
            try {
                // Update bound input buffers in-place - no tensor allocation each step
                std::memcpy(_decEncFrameBuf.data(), encT + t * D, D * sizeof(float));
                // Always seed with blank so the prediction network LSTM starts at
                // its correct initial state. The model naturally emits <|predict_lang|>
                // (updating LSTM to S_A), then <|en|> with LSTM=S_A - which is the
                // exact context it was trained on. Seeding directly with <|predict_lang|>
                // gives LSTM=0 at that step, a context never seen in training, so the
                // model fails to emit the language token.
                _decTargetBuf[0] = static_cast<int32_t>(
                    tokenInfos.empty() ? _blankIdx : tokenInfos.back().token);

                // Run with pre-bound buffers - no tensor creation, no name matching overhead
                _decoderJoint->Run(runOpts, *_decIoBinding);

                const float* logits = _decLogitsBuf.data();

                // Pass 1: branchless top-2 over vocab - compiler-vectorisable
                float best1 = -std::numeric_limits<float>::infinity();
                float best2 = -std::numeric_limits<float>::infinity();
                int bestToken = 0;
                for (int v = 0; v < _vocabSize; v++) {
                    float val = logits[v];
                    if (val >= best1) { best2 = best1; best1 = val; bestToken = v; }
                    else if (val > best2) best2 = val;
                }

                // Pass 2: duration argmax (kept separate so Pass 1 can auto-vectorise cleanly)
                int duration = 0;
                if (numDurations > 0) {
                    float bestDurVal = logits[_vocabSize];
                    for (int d = 1; d < numDurations; d++) {
                        if (logits[_vocabSize + d] > bestDurVal) {
                            bestDurVal = logits[_vocabSize + d];
                            duration = d;
                        }
                    }
                }

                if (bestToken != _blankIdx) {
                    // Non-blank: emit token, record frame index and per-token confidence
                    float tokenConf = static_cast<float>(
                        1.0 / (1.0 + std::exp(-(static_cast<double>(best1) - best2))));
                    tokenInfos.push_back({bestToken, t, tokenConf});
                    emitted++;

                    sumLogProb += std::log(static_cast<double>(tokenConf));
                    numEmitted++;

                    // Propagate ORT's output states into next step's input buffers
                    std::memcpy(_decS1InBuf.data(), _decS1OutBuf.data(), stateSize * sizeof(float));
                    std::memcpy(_decS2InBuf.data(), _decS2OutBuf.data(), stateSize * sizeof(float));
                }
                // else: blank - keep previous input states unchanged

                // Advance time
                if (duration > 0) {
                    t += duration;
                    emitted = 0;
                } else if (bestToken == _blankIdx || emitted >= _maxTokensPerStep) {
                    t += 1;
                    emitted = 0;
                }
                // else: non-blank with duration=0, stay at same t

            } catch (const Ort::Exception& e) {
                spdlog::error("[Parakeet] Decode step error at t={}: {}", t, e.what());
                break;
            }
        }

        // Confidence = geometric mean of per-token confidences
        float confidence = numEmitted > 0
            ? static_cast<float>(std::exp(sumLogProb / numEmitted))
            : 0.0f;

        spdlog::debug("[Parakeet] Decoded {} tokens from {} encoder frames, confidence={:.3f}",
                      tokenInfos.size(), encLen, confidence);
        return {tokenInfos, confidence};
    }

    // --- Build word-level results from decoded token infos ---
    // langEmissions: (frameIndex, LanguageTag) pairs in frame order - used to assign
    // each word the language that was most recently active at its start frame.
    std::vector<va::audio::TranscriptionResult::Word> buildWords(
        const std::vector<DecodeResult::TokenInfo>& tokenInfos,
        double msPerFrame,
        const std::vector<std::pair<int, va::audio::TranscriptionResult::LanguageTag>>& langEmissions = {})
    {
        struct WordSegment {
            std::string text;
            int startFrame = 0;
            int endFrame = 0;
            float sumConf = 0.0f;
            int count = 0;
        };

        std::vector<WordSegment> segments;
        segments.reserve(tokenInfos.size() / 3 + 1);

        for (const auto& ti : tokenInfos) {
            auto it = _vocab.find(ti.token);
            if (it == _vocab.end()) continue;
            const std::string& piece = it->second;
            if (piece.empty()) continue;

            // SentencePiece word-initial pieces are prefixed with a space (converted from ▁)
            bool isWordStart = segments.empty() || piece[0] == ' ';

            if (isWordStart) {
                segments.push_back({});
                segments.back().startFrame = ti.frameIndex;
                segments.back().text = (piece[0] == ' ') ? piece.substr(1) : piece;
            } else {
                segments.back().text += piece;
            }

            auto& seg = segments.back();
            seg.endFrame = ti.frameIndex;
            seg.sumConf += ti.tokenConfidence;
            seg.count++;
        }

        std::vector<va::audio::TranscriptionResult::Word> words;
        words.reserve(segments.size());

        for (const auto& seg : segments) {
            if (seg.text.empty()) continue;
            va::audio::TranscriptionResult::Word w;
            w.text = seg.text;
            w.startTimeMs = static_cast<int64_t>(seg.startFrame * msPerFrame);
            w.endTimeMs = static_cast<int64_t>((seg.endFrame + 1) * msPerFrame);
            w.confidence = seg.count > 0 ? seg.sumConf / seg.count : 0.0f;
            // Tag word with the most recent language emitted at or before its start frame.
            for (auto it = langEmissions.rbegin(); it != langEmissions.rend(); ++it) {
                if (it->first <= seg.startFrame) {
                    w.languageTags = {it->second};
                    break;
                }
            }
            words.push_back(std::move(w));
        }

        return words;
    }

    // --- Token IDs to text ---
    std::string decodeTokens(const std::vector<int>& tokens) {
        std::string raw;
        raw.reserve(tokens.size() * 5); // heuristic: avg ~5 bytes per SentencePiece token
        for (int id : tokens) {
            auto it = _vocab.find(id);
            if (it != _vocab.end()) raw += it->second;
        }
        // Trim leading/trailing whitespace
        size_t start = raw.find_first_not_of(' ');
        size_t end = raw.find_last_not_of(' ');
        if (start == std::string::npos) return "";
        return raw.substr(start, end - start + 1);
    }

    // --- Members ---
    mutable std::mutex _mutex;
    va::metrics::TimingObserverInterface* _timingObserver{nullptr};
    std::string _modelDir;
    std::unique_ptr<Ort::Env> _env;
    std::unique_ptr<Ort::Session> _preprocessor;
    std::unique_ptr<Ort::Session> _encoder;
    std::unique_ptr<Ort::Session> _decoderJoint;
    ModelIO _prepIO, _encIO, _decIO;

    // Cached MemoryInfo - shared across preprocess/encode/tdtDecode (avoids per-call reconstruction)
    Ort::MemoryInfo _memInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    std::unordered_map<int, std::string> _vocab;
    std::unordered_map<int, std::string> _languageTokenIds; // token id -> ISO 639-1 language code
    std::unordered_set<int> _specialTokenIds;               // token ids to skip from transcribed text
    int _vocabSize = 0;
    int _blankIdx = -1;
    int _predictLangIdx = -1; // <|predict_lang|> token id; seeds TDT decoder for language detection
    int _stateLayers = 0;
    int _stateHidden = 0;
    int _maxTokensPerStep = 10;

    // IoBinding infrastructure for decoder-joint - pre-allocated at init, reused every decode step
    std::unique_ptr<Ort::IoBinding> _decIoBinding;
    std::vector<float>   _decEncFrameBuf;   // [D]   - single encoder frame slice per step
    std::vector<int32_t> _decTargetBuf;     // [1]   - prev token
    int32_t              _decTLenBuf = 1;   // constant target_length (always 1)
    std::vector<float>   _decS1InBuf;       // [L*H] - input state, zeroed per utterance
    std::vector<float>   _decS2InBuf;
    std::vector<float>   _decS1OutBuf;      // [L*H] - output state written by ORT each step
    std::vector<float>   _decS2OutBuf;
    std::vector<float>   _decLogitsBuf;     // [vocabSize + numDurations]
    int _encoderHiddenDim = 0;             // D, queried from encoder output shape at init
    int _decTotalOutDim   = 0;             // vocabSize + numDurations, queried at init

    // Decoder-joint I/O index mapping (resolved by name at init)
    int _decIdxEnc = 0, _decIdxTarg = 1, _decIdxTlen = 2, _decIdxS1 = 3, _decIdxS2 = 4;
    int _decOutLogits = 0, _decOutS1 = 1, _decOutS2 = 2;

    // Reusable inference buffers - avoid per-inference heap allocation.
    std::vector<float> _featuresBuffer;      // preprocess() output [1, 128, T]
    std::vector<float> _encOutBuffer;        // encode() output     [1, D, T']
    std::vector<float> _encTransposeBuffer;  // transposed encoder  [T', D]

    // --- Incremental inference cache (partials only; cleared on every final) ---
    // Number of mel boundary frames to recompute at the slice boundary.
    // The mel window (~25ms) extends kMelBoundaryFrames*10ms before the slice start,
    // so those frames would be zero-padded if not recomputed.
    static constexpr int64_t kMelBoundaryFrames = 4;

    // Mel preprocessing cache
    std::vector<float> _incrFeatCache;    // stable mel frames from last partial [stableLen * 128]
    std::vector<float> _incrMelTmpBuf;    // scratch for stitching in preprocessIncremental()
    int64_t            _incrFeatLen = 0;  // stable frame count (= featLen - kMelBoundaryFrames)
    size_t             _incrWavLen  = 0;  // audio sample count at last partial preprocess
    int                _incrHop     = 0;  // mel hop in samples, derived as numSamples/featLen

    // Encoder output cache (stored transposed [T', D] for O(1) append along T')
    std::vector<float> _incrEncCache;     // combined transposed encoder frames from last partial
    int64_t            _incrEncLen = 0;   // number of cached encoder frames (T'_prev)
    int64_t            _incrMelLen = 0;   // mel frame count that produced _incrEncCache
};
