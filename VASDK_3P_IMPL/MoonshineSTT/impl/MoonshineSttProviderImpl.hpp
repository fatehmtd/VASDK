#pragma once

#include <onnxruntime_cxx_api.h>
#include "bin-tokenizer.h"
#include <spdlog/spdlog.h>
#include <VASDK/CommonAudioUtils.h>
#include <VASDK/metrics/MetricsCollector.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Moonshine Base model constants
// ---------------------------------------------------------------------------
static constexpr int kMoonshineNumLayers         = 8;
static constexpr int kMoonshineNumKvHeads        = 8;
static constexpr int kMoonshineHeadDim           = 52;
static constexpr int64_t kMoonshineDecoderBOS    = 1;
static constexpr int64_t kMoonshineDecoderEOS    = 2;

// ---------------------------------------------------------------------------
// Helpers (file-local, defined inline so multiple TUs can include this header)
// ---------------------------------------------------------------------------

namespace moonshine_impl {

inline size_t convertToFloatInto(const uint8_t* data, size_t size,
                                  va::audio::AudioFormat format,
                                  float* out, size_t outCapacity)
{
    switch (format) {
    case va::audio::AudioFormat::SHORT: {
        if (size % sizeof(int16_t) != 0) size -= size % sizeof(int16_t);
        size_t n = std::min(size / sizeof(int16_t), outCapacity);
        for (size_t i = 0; i < n; ++i) {
            int16_t s;
            std::memcpy(&s, data + i * sizeof(int16_t), sizeof(int16_t));
            out[i] = static_cast<float>(s) / 32768.0f;
        }
        return n;
    }
    case va::audio::AudioFormat::FLOAT: {
        if (size % sizeof(float) != 0) size -= size % sizeof(float);
        size_t n = std::min(size / sizeof(float), outCapacity);
        std::memcpy(out, data, n * sizeof(float));
        return n;
    }
    case va::audio::AudioFormat::INT: {
        if (size % sizeof(int32_t) != 0) size -= size % sizeof(int32_t);
        size_t n = std::min(size / sizeof(int32_t), outCapacity);
        for (size_t i = 0; i < n; ++i) {
            int32_t s;
            std::memcpy(&s, data + i * sizeof(int32_t), sizeof(int32_t));
            out[i] = static_cast<float>(s) / 2147483648.0f;
        }
        return n;
    }
    case va::audio::AudioFormat::BYTE: {
        size_t n = std::min(size, outCapacity);
        for (size_t i = 0; i < n; ++i)
            out[i] = (static_cast<float>(data[i]) - 128.0f) / 128.0f;
        return n;
    }
    default:
        spdlog::error("[MoonshineStt] Unknown audio format");
        return 0;
    }
}

inline int64_t argmaxLastLogit(const float* logits, size_t vocabSize)
{
    int64_t best = 0;
    float   bestVal = logits[0];
    for (size_t i = 1; i < vocabSize; ++i) {
        if (logits[i] > bestVal) { bestVal = logits[i]; best = static_cast<int64_t>(i); }
    }
    return best;
}

} // namespace moonshine_impl

// ---------------------------------------------------------------------------
// MoonshineSttProviderImpl
// All ORT and tokenizer state + inference logic lives here.
// ---------------------------------------------------------------------------

class MoonshineSttProviderImpl {
public:
    // Result returned from runInference()
    struct InferResult {
        std::string text;
        float       confidence = 0.0f;
    };

    MoonshineSttProviderImpl() = default;
    ~MoonshineSttProviderImpl() = default;

    // Non-copyable
    MoonshineSttProviderImpl(const MoonshineSttProviderImpl&)            = delete;
    MoonshineSttProviderImpl& operator=(const MoonshineSttProviderImpl&) = delete;

    bool initialize(const std::string& modelDir)
    {
        const std::string encoderPath   = modelDir + "/encoder_model.onnx";
        const std::string decoderPath   = modelDir + "/decoder_model_merged.onnx";
        const std::string tokenizerPath = modelDir + "/tokenizer.bin";

        for (const auto& p : { encoderPath, decoderPath, tokenizerPath }) {
            if (!std::filesystem::exists(p)) {
                spdlog::error("[MoonshineStt] Model file not found: {}", p);
                return false;
            }
        }

        _sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);
        _sessionOptions.SetIntraOpNumThreads(1);

#ifdef _WIN32
        _encoderSession = std::make_unique<Ort::Session>(
            _env,
            std::wstring(encoderPath.begin(), encoderPath.end()).c_str(),
            _sessionOptions);
        _decoderSession = std::make_unique<Ort::Session>(
            _env,
            std::wstring(decoderPath.begin(), decoderPath.end()).c_str(),
            _sessionOptions);
#else
        _encoderSession = std::make_unique<Ort::Session>(
            _env, encoderPath.c_str(), _sessionOptions);
        _decoderSession = std::make_unique<Ort::Session>(
            _env, decoderPath.c_str(), _sessionOptions);
#endif

        _tokenizer = std::make_unique<BinTokenizer>(tokenizerPath.c_str());
        return true;
    }

    // Run a full encoder → greedy KV-cache decoder pass.
    // audioFormatInt matches va::audio::AudioFormat cast to int.
    InferResult runInference(const float* audioData, size_t numSamples,
                             int sampleRate, float maxTokensPerSecond,
                             va::metrics::TimingObserverInterface* timingObserver = nullptr)
    {
        if (!_encoderSession || !_decoderSession || numSamples == 0)
            return {};

        Ort::AllocatorWithDefaultOptions allocator;

        //  Encoder 
        const size_t encInCount  = _encoderSession->GetInputCount();
        const size_t encOutCount = _encoderSession->GetOutputCount();

        std::vector<std::string> encInNames(encInCount);
        std::vector<std::string> encOutNames(encOutCount);
        for (size_t i = 0; i < encInCount;  ++i)
            encInNames[i]  = _encoderSession->GetInputNameAllocated(i,  allocator).get();
        for (size_t i = 0; i < encOutCount; ++i)
            encOutNames[i] = _encoderSession->GetOutputNameAllocated(i, allocator).get();

        std::vector<const char*> encInPtrs(encInCount);
        std::vector<const char*> encOutPtrs(encOutCount);
        for (size_t i = 0; i < encInCount;  ++i) encInPtrs[i]  = encInNames[i].c_str();
        for (size_t i = 0; i < encOutCount; ++i) encOutPtrs[i] = encOutNames[i].c_str();

        std::vector<Ort::Value> encInputs;
        {
            std::array<int64_t, 2> shape{ 1, static_cast<int64_t>(numSamples) };
            encInputs.push_back(Ort::Value::CreateTensor<float>(
                _memInfo, const_cast<float*>(audioData), numSamples,
                shape.data(), shape.size()));
        }
        std::vector<int64_t> attnMask;
        if (encInCount > 1) {
            attnMask.assign(numSamples, 1LL);
            std::array<int64_t, 2> shape{ 1, static_cast<int64_t>(numSamples) };
            encInputs.push_back(Ort::Value::CreateTensor<int64_t>(
                _memInfo, attnMask.data(), attnMask.size(),
                shape.data(), shape.size()));
        }

        auto t0Enc = std::chrono::steady_clock::now();
        std::vector<Ort::Value> encOutputs;
        try {
            encOutputs = _encoderSession->Run(
                Ort::RunOptions{}, encInPtrs.data(), encInputs.data(), encInCount,
                encOutPtrs.data(), encOutCount);
        }
        catch (const Ort::Exception& e) {
            spdlog::error("[MoonshineStt] Encoder error: {}", e.what());
            return {};
        }
        if (timingObserver) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0Enc).count();
            timingObserver->onTiming("encoder", ms);
        }

        // Encoder output: last_hidden_state [1, frames, encoder_dim]
        const float* hiddenData  = encOutputs[0].GetTensorData<float>();
        auto hiddenShape         = encOutputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int64_t encFrames  = hiddenShape[1];
        const int64_t encDim     = hiddenShape[2];
        const size_t  hiddenSize = static_cast<size_t>(encFrames * encDim);

        //  Decoder 
        const size_t decInCount  = _decoderSession->GetInputCount();
        const size_t decOutCount = _decoderSession->GetOutputCount();

        std::vector<std::string> decInNames(decInCount);
        std::vector<std::string> decOutNames(decOutCount);
        for (size_t i = 0; i < decInCount;  ++i)
            decInNames[i]  = _decoderSession->GetInputNameAllocated(i,  allocator).get();
        for (size_t i = 0; i < decOutCount; ++i)
            decOutNames[i] = _decoderSession->GetOutputNameAllocated(i, allocator).get();

        std::unordered_map<std::string, size_t> decInIdx, decOutIdx;
        for (size_t i = 0; i < decInCount;  ++i) decInIdx[decInNames[i]]  = i;
        for (size_t i = 0; i < decOutCount; ++i) decOutIdx[decOutNames[i]] = i;

        std::vector<const char*> decInPtrs(decInCount);
        std::vector<const char*> decOutPtrs(decOutCount);
        for (size_t i = 0; i < decInCount;  ++i) decInPtrs[i]  = decInNames[i].c_str();
        for (size_t i = 0; i < decOutCount; ++i) decOutPtrs[i] = decOutNames[i].c_str();

        // Initial KV-cache: all zeros, shape [1, kNumKvHeads, 1, kHeadDim]
        const size_t kvElemCount = static_cast<size_t>(kMoonshineNumKvHeads * kMoonshineHeadDim);
        std::vector<float> kvZero(kvElemCount, 0.0f);

        struct KVEntry { std::vector<float> data; };
        std::unordered_map<std::string, KVEntry> pastKV;
        for (int layer = 0; layer < kMoonshineNumLayers; ++layer) {
            for (const char* attn : { "decoder", "encoder" }) {
                for (const char* kv   : { "key",     "value"   }) {
                    pastKV["past_key_values." + std::to_string(layer)
                           + "." + attn + "." + kv].data = kvZero;
                }
            }
        }

        // Decode limits
        const float audioDurSec = static_cast<float>(numSamples)
                                  / static_cast<float>(sampleRate > 0 ? sampleRate : 16000);
        const int maxLen = std::max(1, static_cast<int>(
            std::ceil(audioDurSec * maxTokensPerSecond)));

        std::vector<int64_t> tokens;
        tokens.reserve(static_cast<size_t>(maxLen) + 2);

        std::vector<float>    encoderHidden(hiddenData, hiddenData + hiddenSize);
        std::array<int64_t,3> encHidShape{ 1, encFrames, encDim };

        int64_t inputId = kMoonshineDecoderBOS;

        auto t0Dec = std::chrono::steady_clock::now();

        for (int step = 0; step < maxLen; ++step) {
            const bool useCache = (step > 0);

            // Ort::Value has no default constructor - use an ordered map then drain.
            std::map<size_t, Ort::Value> inputMap;

            int64_t idScalar    = inputId;
            bool    cacheScalar = useCache;

            // input_ids [1, 1]
            {
                std::array<int64_t, 2> shape{ 1, 1 };
                inputMap.emplace(decInIdx.at("input_ids"),
                    Ort::Value::CreateTensor<int64_t>(
                        _memInfo, &idScalar, 1, shape.data(), shape.size()));
            }
            // encoder_hidden_states [1, frames, encoder_dim]
            {
                inputMap.emplace(decInIdx.at("encoder_hidden_states"),
                    Ort::Value::CreateTensor<float>(
                        _memInfo, encoderHidden.data(), hiddenSize,
                        encHidShape.data(), encHidShape.size()));
            }
            // use_cache_branch [1]
            {
                std::array<int64_t, 1> shape{ 1 };
                inputMap.emplace(decInIdx.at("use_cache_branch"),
                    Ort::Value::CreateTensor<bool>(
                        _memInfo, &cacheScalar, 1, shape.data(), shape.size()));
            }
            // past_key_values.*
            for (int layer = 0; layer < kMoonshineNumLayers; ++layer) {
                for (const char* attn : { "decoder", "encoder" }) {
                    for (const char* kv : { "key", "value" }) {
                        std::string name = "past_key_values." + std::to_string(layer)
                                           + "." + attn + "." + kv;
                        auto idxIt = decInIdx.find(name);
                        if (idxIt == decInIdx.end()) continue;
                        auto& entry  = pastKV.at(name);
                        size_t dSize = entry.data.size();
                        size_t pLen  = dSize / static_cast<size_t>(
                            kMoonshineNumKvHeads * kMoonshineHeadDim);
                        std::array<int64_t, 4> shape{
                            1, kMoonshineNumKvHeads,
                            static_cast<int64_t>(pLen), kMoonshineHeadDim };
                        inputMap.emplace(idxIt->second,
                            Ort::Value::CreateTensor<float>(
                                _memInfo, entry.data.data(), dSize,
                                shape.data(), shape.size()));
                    }
                }
            }

            // Drain map → ordered vector
            std::vector<Ort::Value> decInputs;
            decInputs.reserve(decInCount);
            for (auto& [idx, val] : inputMap)
                decInputs.push_back(std::move(val));

            if (decInputs.size() != decInCount) {
                spdlog::error("[MoonshineStt] Step {}: expected {} inputs, got {}",
                              step, decInCount, decInputs.size());
                break;
            }

            std::vector<Ort::Value> decOutputs;
            try {
                decOutputs = _decoderSession->Run(
                    Ort::RunOptions{}, decInPtrs.data(), decInputs.data(), decInCount,
                    decOutPtrs.data(), decOutCount);
            }
            catch (const Ort::Exception& e) {
                spdlog::error("[MoonshineStt] Decoder step {}: {}", step, e.what());
                break;
            }

            // Argmax over last logit
            {
                const float* logitData = decOutputs[decOutIdx.at("logits")].GetTensorData<float>();
                auto logitShape = decOutputs[decOutIdx.at("logits")].GetTensorTypeAndShapeInfo().GetShape();
                size_t vocab  = static_cast<size_t>(logitShape[2]);
                size_t seqLen = static_cast<size_t>(logitShape[1]);
                inputId = moonshine_impl::argmaxLastLogit(logitData + (seqLen - 1) * vocab, vocab);
            }

            tokens.push_back(inputId);
            if (inputId == kMoonshineDecoderEOS) break;

            // Update KV-cache: always decoder KVs, encoder KVs only on step 0
            for (int layer = 0; layer < kMoonshineNumLayers; ++layer) {
                for (const char* attn : { "decoder", "encoder" }) {
                    if (useCache && std::string(attn) == "encoder") continue;
                    for (const char* kv : { "key", "value" }) {
                        std::string presName = "present." + std::to_string(layer)
                                              + "." + attn + "." + kv;
                        std::string pastName = "past_key_values." + std::to_string(layer)
                                              + "." + attn + "." + kv;
                        auto outIt = decOutIdx.find(presName);
                        if (outIt == decOutIdx.end()) continue;
                        const float* src = decOutputs[outIt->second].GetTensorData<float>();
                        auto shape = decOutputs[outIt->second].GetTensorTypeAndShapeInfo().GetShape();
                        size_t count = 1;
                        for (auto d : shape) count *= static_cast<size_t>(d);
                        pastKV.at(pastName).data.assign(src, src + count);
                    }
                }
            }
        } // decode loop

        if (timingObserver) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0Dec).count();
            timingObserver->onTiming("decoder", ms);
        }

        InferResult result;
        if (!tokens.empty()) {
            try {
                result.text = _tokenizer->tokens_to_text<int64_t>(tokens, true);
            }
            catch (const std::exception& e) {
                spdlog::warn("[MoonshineStt] Tokenizer error: {}", e.what());
            }
            size_t effective = tokens.size();
            if (tokens.back() == kMoonshineDecoderEOS) --effective;
            result.confidence = 1.0f - std::min(1.0f,
                static_cast<float>(effective) / static_cast<float>(
                    std::max(1, static_cast<int>(
                        std::ceil(audioDurSec * maxTokensPerSecond)))));
        }
        return result;
    }

private:
    Ort::Env            _env{ ORT_LOGGING_LEVEL_WARNING, "MoonshineStt" };
    Ort::SessionOptions _sessionOptions{};
    Ort::MemoryInfo     _memInfo{ Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault) };
    std::unique_ptr<Ort::Session> _encoderSession;
    std::unique_ptr<Ort::Session> _decoderSession;
    std::unique_ptr<BinTokenizer> _tokenizer;
};
