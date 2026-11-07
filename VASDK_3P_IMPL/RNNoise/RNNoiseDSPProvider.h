#pragma once

#include <VASDK/GenericAudioDSPProvider.h>
#include "vasdk_rnnoise_export.h"
#include <string>
#include <vector>
#include <cstddef>

// Forward-declare RNNoise C types to avoid including rnnoise.h in this header.
struct DenoiseState;
struct RNNModel;

namespace va::thirdparty::audio {

/** Settings of RNNoiseDSPProvider; every field has a default. */
struct RNNoiseDSPOptions {
    /** A custom RNNoise model file (.rnnn); empty uses the model built into the library. */
    std::string modelPath;
    /** RNNoise voice probability (0-1) under which a frame counts as silence, exposed through
     *  getLastVADProbability(); 0 disables it. */
    float vadThreshold = 0.0f;
    /** Scale the output so its peak stays under normalizationMultiplier of full scale. */
    bool normalizeOutput = false;
    /** Headroom for normalizeOutput: 0.8 is 80% of full scale. */
    float normalizationMultiplier = 0.8f;
};

/**
 * @brief Noise suppression provider using RNNoise (https://github.com/xiph/rnnoise).
 *
 * RNNoiseDSPProvider implements va::audio::GenericAudioDSPProvider using the
 * RNNoise recurrent-neural-network noise suppressor developed by Mozilla /
 * Jean-Marc Valin.
 *
 * RNNoise operates internally at 48 kHz with fixed 480-sample frames (10 ms).
 * This implementation transparently resamples from any source sample rate to
 * 48 kHz and back using linear interpolation - no integer-ratio constraint.
 *
 * Accepted sample formats: SHORT (int16), FLOAT (float32), INT (int32).
 * Only mono audio is supported.
 *
 * An optional custom RNNoise model (.rnnn file) may be supplied; when omitted
 * the weights compiled into the rnnoise library are used.
 *
 * Example:
 * @code
 *   auto provider = std::make_shared<RNNoiseDSPProvider>();
 *   provider->initialize(audioConfig);
 *
 *   auto dspProcessor = std::make_shared<va::pipeline::AudioDSPProcessor>(provider);
 *   dspProcessor->setRawAudioRingBuffer(rawBuffer);
 *   dspProcessor->setCleanAudioRingBuffer(cleanBuffer);
 * @endcode
 */
class VASDK_RNNOISE_EXPORT RNNoiseDSPProvider : public va::audio::GenericAudioDSPProvider {
public:
    explicit RNNoiseDSPProvider(RNNoiseDSPOptions options = {});
    ~RNNoiseDSPProvider() override;

    std::string getName() const override { return "RNNoise"; }

    bool   initialize(const va::audio::AudioConfig& config) override;
    void   release()                                        override;

    size_t processAudio(const char* input, char* output,
                        size_t numSamples)                  override;

    /** @brief RNNoise's voice probability for the last processed 10 ms frame. */
    float getLastVADProbability() const { return lastVADProb_; }

private:
    // Linear-interpolation resampling (arbitrary floating-point ratio)
    void upsample(const float* input, size_t inCount,
                  std::vector<float>& output) const;

    void downsample(const float* input48, size_t count48,
                    float* output, size_t outCount) const;

    // internal state
    std::string   modelPath_;
    DenoiseState* _state         = nullptr;
    RNNModel*     model_         = nullptr;
    double        upsampleRatio_ = 3.0;  // RNNOISE_RATE / _config.sampleRate
    float         vadThreshold_  = 0.0f;
    float         lastVADProb_   = 0.0f;
    bool          normalizeOutput_ = false;
    float         normalizationMultiplier_ = 0.8f; // headroom to prevent clipping

    // Cross-call accumulation buffers
    std::vector<float> inputBuf48_;      // 48 kHz input not yet processed
    std::vector<float> processedBuf48_;  // processed 48 kHz not yet downsampled
    std::vector<float> tempFrame48_;     // one RNNoise frame (480 samples)
    std::vector<float> floatIn_;         // raw → float scratch
    std::vector<float> floatOut_;        // float → raw scratch
};

} // namespace va::thirdparty::audio
