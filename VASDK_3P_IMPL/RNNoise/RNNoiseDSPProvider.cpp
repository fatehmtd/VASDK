#include "RNNoiseDSPProvider.h"

#include <VASDK/AudioSIMD.h>
#include <rnnoise.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// 
// RNNoise constants
// 

static constexpr int   RNNOISE_RATE = 48000;
static constexpr int   RNNOISE_FRAME_SIZE = 480;   // 10 ms at 48 kHz

// RNNoise API uses floats in the range ±32768 (not normalised ±1).
static constexpr float RNNOISE_SCALE = 32768.0f;

// 
// Construction / destruction
// 

RNNoiseDSPProvider::RNNoiseDSPProvider(const std::string& modelPath)
    : modelPath_(modelPath)
{
}

RNNoiseDSPProvider::~RNNoiseDSPProvider()
{
    release();
}

// 
// Lifecycle
// 

bool RNNoiseDSPProvider::initialize(const va::audio::AudioConfig& config)
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

void RNNoiseDSPProvider::release()
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

void RNNoiseDSPProvider::upsample(const float* input, size_t inCount,
    std::vector<float>& output) const
{
    if (upsampleRatio_ == 1.0) {
        const size_t base = output.size();
        output.resize(base + inCount);
        std::copy(input, input + inCount, output.begin() + base);
        return;
    }

    const size_t outCount = static_cast<size_t>(
        std::ceil(static_cast<double>(inCount) * upsampleRatio_));

    const size_t base = output.size();
    output.resize(base + outCount);

    for (size_t j = 0; j < outCount; ++j) {
        const double srcPos = static_cast<double>(j) / upsampleRatio_;
        const size_t idx = static_cast<size_t>(srcPos);
        const double frac = srcPos - static_cast<double>(idx);

        const float s0 = (idx < inCount) ? input[idx] : input[inCount - 1];
        const float s1 = (idx + 1 < inCount) ? input[idx + 1] : s0;
        output[base + j] = static_cast<float>(s0 + frac * (s1 - s0));
    }
}

void RNNoiseDSPProvider::downsample(const float* input48, size_t count48,
    float* output, size_t outCount) const
{
    if (upsampleRatio_ == 1.0) {
        std::copy(input48, input48 + outCount, output);
        return;
    }

    for (size_t i = 0; i < outCount; ++i) {
        const double srcPos = static_cast<double>(i) * upsampleRatio_;
        const size_t idx = static_cast<size_t>(srcPos);
        const double frac = srcPos - static_cast<double>(idx);

        const float s0 = (idx < count48) ? input48[idx] : input48[count48 - 1];
        const float s1 = (idx + 1 < count48) ? input48[idx + 1] : s0;
        output[i] = static_cast<float>(s0 + frac * (s1 - s0));
    }
}

// 
// processAudio - format conversion, resampling, denoising
// 

size_t RNNoiseDSPProvider::processAudio(const char* input, char* output,
    size_t numSamples)
{
    if (!_state || numSamples == 0) {
        if (output != input)
            std::memcpy(output, input, numSamples * _config.getSampleSizeInBytes());
        return numSamples;
    }

    //  Step 1: raw bytes → normalised float [-1, 1] 
    floatIn_.resize(numSamples);

    switch (_config.format) {
    case va::audio::AudioFormat::SHORT:
        AudioSIMD::convertInt16ToFloat(
            reinterpret_cast<const int16_t*>(input), floatIn_.data(), numSamples);
        break;
    case va::audio::AudioFormat::FLOAT:
        std::memcpy(floatIn_.data(), input, numSamples * sizeof(float));
        break;
    case va::audio::AudioFormat::INT:
        AudioSIMD::convertInt32ToFloat(
            reinterpret_cast<const int32_t*>(input), floatIn_.data(), numSamples);
        break;
    default:
        spdlog::warn("[RNNoise] Unsupported audio format - passing through");
        std::memcpy(output, input, numSamples * _config.getSampleSizeInBytes());
        return numSamples;
    }

    //  Step 2: upsample to 48 kHz 
    upsample(floatIn_.data(), numSamples, inputBuf48_);

    //  Step 3: process complete 480-sample RNNoise frames 
    size_t consumed48 = 0;
    while (consumed48 + RNNOISE_FRAME_SIZE <= inputBuf48_.size()) {
        for (int i = 0; i < RNNOISE_FRAME_SIZE; ++i)
            tempFrame48_[i] = inputBuf48_[consumed48 + i] * RNNOISE_SCALE;

        lastVADProb_ = rnnoise_process_frame(_state,
            tempFrame48_.data(),
            tempFrame48_.data());

        const size_t writeBase = processedBuf48_.size();
        processedBuf48_.resize(writeBase + RNNOISE_FRAME_SIZE);
        for (int i = 0; i < RNNOISE_FRAME_SIZE; ++i)
            processedBuf48_[writeBase + i] = tempFrame48_[i] / RNNOISE_SCALE;

        consumed48 += RNNOISE_FRAME_SIZE;
    }

    if (consumed48 > 0)
        inputBuf48_.erase(inputBuf48_.begin(),
            inputBuf48_.begin() + consumed48);

    //  Step 4: downsample processed 48 kHz → source rate 
    const size_t available48 = processedBuf48_.size();
    const size_t available16 = static_cast<size_t>(
        static_cast<double>(available48) / upsampleRatio_);

    const size_t fromProcessed = std::min(available16, numSamples);
    const size_t fromPassthru = numSamples - fromProcessed;

    floatOut_.resize(numSamples);

    if (fromProcessed > 0) {
        const size_t use48 = std::min(
            static_cast<size_t>(std::ceil(
                static_cast<double>(fromProcessed) * upsampleRatio_)),
            available48);

        downsample(processedBuf48_.data(), use48, floatOut_.data(), fromProcessed);
        processedBuf48_.erase(processedBuf48_.begin(),
            processedBuf48_.begin() + use48);
    }

    if (fromPassthru > 0) {
        std::copy(floatIn_.begin() + fromProcessed,
            floatIn_.begin() + numSamples,
            floatOut_.begin() + fromProcessed);
    }

    //  Step 5: normalize output (optional) 

    if (normalizeOutput_) {
        float maxValue = std::abs(floatOut_[0]);
        for (size_t i = 1; i < numSamples; ++i) {
            if (std::abs(floatOut_[i]) > maxValue) {
                maxValue = std::abs(floatOut_[i]);
            }
        }
        float multiplier = normalizationMultiplier_; // headroom to prevent clipping
        maxValue = multiplier / maxValue;
        for (size_t i = 0; i < numSamples; ++i) {
            floatOut_[i] *= maxValue;
        }
    }

    //  Step 6: float → original format 

    switch (_config.format) {
    case va::audio::AudioFormat::SHORT:
        AudioSIMD::convertFloatToInt16(
            floatOut_.data(), reinterpret_cast<int16_t*>(output), numSamples);
        break;
    case va::audio::AudioFormat::FLOAT:
        std::memcpy(output, floatOut_.data(), numSamples * sizeof(float));
        break;
    case va::audio::AudioFormat::INT:
        AudioSIMD::convertFloatToInt32(
            floatOut_.data(), reinterpret_cast<int32_t*>(output), numSamples);
        break;
    default:
        break;
    }

    return numSamples;
}
