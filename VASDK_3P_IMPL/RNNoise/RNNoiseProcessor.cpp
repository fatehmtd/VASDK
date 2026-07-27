#include "RNNoiseProcessor.h"

#include <rnnoise.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// 
// RNNoise constants
// 

static constexpr int   RNNOISE_RATE       = 48000;
static constexpr int   RNNOISE_FRAME_SIZE = 480;   // 10 ms at 48 kHz

// RNNoise API uses floats in the range ±32768 (not normalised ±1).
static constexpr float RNNOISE_SCALE      = 32768.0f;

// 
// Construction / destruction
// 

RNNoiseProcessor::RNNoiseProcessor(const std::string& modelPath)
    : modelPath_(modelPath)
{
}

RNNoiseProcessor::~RNNoiseProcessor()
{
    release();
}

// 
// Lifecycle
// 

bool RNNoiseProcessor::initialize(const va::audio::AudioConfig& config)
{
    _config = config;

    if (config.channels != 1) {
        spdlog::error("[RNNoise] Only mono audio is supported (channels={})",
                      config.channels);
        return false;
    }

    if (config.sampleRate <= 0) {
        spdlog::error("[RNNoise] Invalid sample rate: {}", config.sampleRate);
        return false;
    }

    upsampleRatio_ = static_cast<double>(RNNOISE_RATE) / config.sampleRate;

    // Load optional custom model
    if (!modelPath_.empty()) {
#ifdef _MSC_VER
        FILE* f = nullptr;
        fopen_s(&f, modelPath_.c_str(), "rb");
#else
        FILE* f = std::fopen(modelPath_.c_str(), "rb");
#endif
        if (!f) {
            spdlog::error("[RNNoise] Could not open model file: {}", modelPath_);
            return false;
        }
        model_ = rnnoise_model_from_file(f);
        std::fclose(f);
        if (!model_) {
            spdlog::error("[RNNoise] Failed to parse model file: {}", modelPath_);
            return false;
        }
        spdlog::info("[RNNoise] Loaded custom model from: {}", modelPath_);
    }

    // nullptr → use the default compiled-in model weights
    _state = rnnoise_create(model_);
    if (!_state) {
        spdlog::error("[RNNoise] rnnoise_create() failed");
        return false;
    }

    tempFrame48_.resize(RNNOISE_FRAME_SIZE);
    inputBuf48_.clear();
    processedBuf48_.clear();

    spdlog::info("[RNNoise] Initialized (sourceRate={} Hz, upsampleRatio={:.4f}x, "
                 "model={})",
                 config.sampleRate, upsampleRatio_,
                 modelPath_.empty() ? "built-in" : modelPath_);
    return true;
}

void RNNoiseProcessor::release()
{
    if (_state) {
        rnnoise_destroy(_state);
        _state = nullptr;
    }
    if (model_) {
        rnnoise_model_free(model_);
        model_ = nullptr;
    }
    inputBuf48_.clear();
    processedBuf48_.clear();
}

// 
// Resampling - linear interpolation, arbitrary floating-point ratio
// 

void RNNoiseProcessor::upsample(const float* input, size_t inCount,
                                std::vector<float>& output) const
{
    if (upsampleRatio_ == 1.0) {
        const size_t base = output.size();
        output.resize(base + inCount);
        std::copy(input, input + inCount, output.begin() + base);
        return;
    }

    // How many 48 kHz samples correspond to inCount source samples?
    const size_t outCount = static_cast<size_t>(
        std::ceil(static_cast<double>(inCount) * upsampleRatio_));

    const size_t base = output.size();
    output.resize(base + outCount);

    for (size_t j = 0; j < outCount; ++j) {
        // Fractional position in the source stream
        const double srcPos = static_cast<double>(j) / upsampleRatio_;
        const size_t idx    = static_cast<size_t>(srcPos);
        const double frac   = srcPos - static_cast<double>(idx);

        const float s0 = (idx     < inCount) ? input[idx]     : input[inCount - 1];
        const float s1 = (idx + 1 < inCount) ? input[idx + 1] : s0;
        output[base + j] = static_cast<float>(s0 + frac * (s1 - s0));
    }
}

void RNNoiseProcessor::downsample(const float* input48, size_t count48,
                                  float* output, size_t outCount) const
{
    if (upsampleRatio_ == 1.0) {
        std::copy(input48, input48 + outCount, output);
        return;
    }

    for (size_t i = 0; i < outCount; ++i) {
        // Fractional position in the 48 kHz buffer
        const double srcPos = static_cast<double>(i) * upsampleRatio_;
        const size_t idx    = static_cast<size_t>(srcPos);
        const double frac   = srcPos - static_cast<double>(idx);

        const float s0 = (idx     < count48) ? input48[idx]     : input48[count48 - 1];
        const float s1 = (idx + 1 < count48) ? input48[idx + 1] : s0;
        output[i] = static_cast<float>(s0 + frac * (s1 - s0));
    }
}

// 
// processAudio - handles raw bytes, format conversion, resampling, denoising
// 

size_t RNNoiseProcessor::processAudio(const char* input, char* output,
                                      size_t numSamples)
{
    if (!_state || numSamples == 0) {
        if (output != input)
            std::memcpy(output, input, numSamples * _config.getSampleSizeInBytes());
        return numSamples;
    }

    //  Step 1: convert raw bytes → normalised float [-1, 1] 
    floatIn_.resize(numSamples);

    switch (_config.format) {
    case va::audio::AudioFormat::SHORT: {
        const auto* src = reinterpret_cast<const int16_t*>(input);
        for (size_t i = 0; i < numSamples; ++i)
            floatIn_[i] = static_cast<float>(src[i]) / 32768.0f;
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        std::memcpy(floatIn_.data(), input, numSamples * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        const auto* src = reinterpret_cast<const int32_t*>(input);
        constexpr float kScale = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < numSamples; ++i)
            floatIn_[i] = static_cast<float>(src[i]) * kScale;
        break;
    }
    default:
        spdlog::warn("[RNNoise] Unsupported audio format - passing through");
        std::memcpy(output, input, numSamples * _config.getSampleSizeInBytes());
        return numSamples;
    }

    //  Step 2: upsample to 48 kHz and append to accumulation buffer 
    upsample(floatIn_.data(), numSamples, inputBuf48_);

    //  Step 3: process all complete 480-sample RNNoise frames 
    size_t consumed48 = 0;
    while (consumed48 + RNNOISE_FRAME_SIZE <= inputBuf48_.size()) {
        // Scale from [-1, 1] to RNNoise range [±32768]
        for (int i = 0; i < RNNOISE_FRAME_SIZE; ++i)
            tempFrame48_[i] = inputBuf48_[consumed48 + i] * RNNOISE_SCALE;

        lastVADProb_ = rnnoise_process_frame(_state,
                                             tempFrame48_.data(),
                                             tempFrame48_.data());

        // Scale back to [-1, 1] and accumulate in the processed buffer
        const size_t writeBase = processedBuf48_.size();
        processedBuf48_.resize(writeBase + RNNOISE_FRAME_SIZE);
        for (int i = 0; i < RNNOISE_FRAME_SIZE; ++i)
            processedBuf48_[writeBase + i] = tempFrame48_[i] / RNNOISE_SCALE;

        consumed48 += RNNOISE_FRAME_SIZE;
    }

    // Discard consumed 48 kHz input; keep leftover for next call
    if (consumed48 > 0)
        inputBuf48_.erase(inputBuf48_.begin(),
                          inputBuf48_.begin() + consumed48);

    //  Step 4: downsample to source rate 
    //
    // Determine how many source-rate samples the available processed 48 kHz
    // samples translate to.  Use floor to avoid reading past the buffer.
    const size_t available48  = processedBuf48_.size();
    const size_t available16  = static_cast<size_t>(
        static_cast<double>(available48) / upsampleRatio_);

    const size_t fromProcessed = std::min(available16, numSamples);
    const size_t fromPassthru  = numSamples - fromProcessed;

    floatOut_.resize(numSamples);

    if (fromProcessed > 0) {
        // Consume exactly as many 48 kHz samples as needed for fromProcessed
        const size_t use48 = std::min(
            static_cast<size_t>(std::ceil(
                static_cast<double>(fromProcessed) * upsampleRatio_)),
            available48);

        downsample(processedBuf48_.data(), use48, floatOut_.data(), fromProcessed);
        processedBuf48_.erase(processedBuf48_.begin(),
                              processedBuf48_.begin() + use48);
    }

    // Any tail samples that don't have processed output yet are passed through
    // (at most a few samples per frame for non-integer ratios).
    if (fromPassthru > 0) {
        std::copy(floatIn_.begin()  + fromProcessed,
                  floatIn_.begin()  + numSamples,
                  floatOut_.begin() + fromProcessed);
    }

    //  Step 5: convert float → original format and write to output 
    switch (_config.format) {
    case va::audio::AudioFormat::SHORT: {
        auto* dst = reinterpret_cast<int16_t*>(output);
        for (size_t i = 0; i < numSamples; ++i) {
            float v = floatOut_[i] * 32768.0f;
            v = std::max(-32768.0f, std::min(32767.0f, v));
            dst[i] = static_cast<int16_t>(v);
        }
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        std::memcpy(output, floatOut_.data(), numSamples * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        auto* dst = reinterpret_cast<int32_t*>(output);
        constexpr float kScale = 2147483648.0f;
        for (size_t i = 0; i < numSamples; ++i) {
            float v = floatOut_[i] * kScale;
            v = std::max(-2147483648.0f, std::min(2147483647.0f, v));
            dst[i] = static_cast<int32_t>(v);
        }
        break;
    }
    default:
        break;
    }

    return numSamples;
}
