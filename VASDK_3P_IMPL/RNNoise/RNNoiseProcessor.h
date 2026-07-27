#pragma once

#include <VASDK/GenericAudioDSPProvider.h>
#include "vasdk_rnnoise_export.h"
#include <string>
#include <vector>
#include <cstddef>

// Forward-declare RNNoise C types to avoid including rnnoise.h in this header.
struct DenoiseState;
struct RNNModel;

/**
 * @brief Noise suppression provider using RNNoise (https://github.com/xiph/rnnoise).
 *
 * RNNoiseProcessor implements va::audio::AudioDSPProcessor using the RNNoise
 * recurrent-neural-network noise suppressor developed by Mozilla / Jean-Marc Valin.
 *
 * RNNoise operates internally at 48 kHz with fixed 480-sample frames (10 ms).
 * This implementation transparently resamples from any source sample rate to
 * 48 kHz before processing and resamples back afterwards using linear
 * interpolation.  All sample rates are supported - no integer-ratio constraint.
 *
 * Audio is accepted and returned as raw bytes in the format specified by
 * initialize() (SHORT, FLOAT, or INT).  Format conversion is handled internally.
 * Only mono audio is supported.
 *
 * A custom RNNoise model (.rnnn file) can optionally be supplied; when omitted
 * the weights compiled into the rnnoise library are used.
 *
 * Configuration (set before initialize()):
 *   setModelPath(path)     - path to a custom RNNoise model file
 *   setVADThreshold(t)     - frames below this VAD probability are suppressed harder
 *
 * Example:
 * @code
 *   auto rnnoise = std::make_shared<RNNoiseProcessor>();
 *   // rnnoise->setModelPath("/path/to/custom.rnnn");  // optional
 *
 *   auto dspProvider = std::make_shared<va::audio::AudioDSPProvider>();
 *   dspProvider->addProcessor(rnnoise);
 *   dspProvider->initialize(audioConfig, ringBufferCapacity);
 *   localTransport->setAudioDSPProvider(dspProvider);
 *
 *   vadProcessor->setRingBuffer(dspProvider->getCleanRingBuffer());
 * @endcode
 */
class VASDK_RNNOISE_EXPORT RNNoiseProcessor : public va::audio::GenericAudioDSPProvider {
public:
    /**
     * @brief Construct with an optional custom model path.
     * @param modelPath  Path to a .rnnn RNNoise model file.
     *                   Leave empty (default) to use the compiled-in model.
     */
    explicit RNNoiseProcessor(const std::string& modelPath = "");
    ~RNNoiseProcessor() override;

    std::string getName() const override { return "RNNoise"; }

    bool   initialize(const va::audio::AudioConfig& config) override;
    void   release()   override;

    /**
     * @brief Process audio bytes through RNNoise.
     *
     * @p input and @p output are raw bytes in the format supplied to
     * initialize() (SHORT, FLOAT, or INT).  Format conversion, resampling to
     * 48 kHz, and RNNoise frame processing are all handled internally.
     */
    size_t processAudio(const char* input, char* output,
                        size_t numSamples) override;

    /** @brief Set the path to a custom RNNoise model file (call before initialize()). */
    void setModelPath(const std::string& path) { modelPath_ = path; }

    /**
     * @brief Set the VAD probability threshold (0.0 = disabled, default).
     *
     * Frames whose RNNoise VAD probability falls below this threshold are
     * treated as silence; their output energy is reduced further.
     */
    void setVADThreshold(float threshold) { vadThreshold_ = threshold; }

    /** @brief VAD probability returned for the last processed RNNoise frame. */
    float getLastVADProbability() const { return lastVADProb_; }

private:
    //  Internal resampling (linear interpolation, arbitrary ratio) 
    void upsample(const float* input, size_t inCount,
                  std::vector<float>& output) const;

    void downsample(const float* input48, size_t count48,
                    float* output, size_t outCount) const;

    //  State 
    std::string   modelPath_;
    DenoiseState* _state         = nullptr;
    RNNModel*     model_         = nullptr;
    double        upsampleRatio_ = 3.0;  // RNNOISE_RATE / _config.sampleRate
    float         vadThreshold_  = 0.0f;
    float         lastVADProb_   = 0.0f;

    // Accumulation buffers across processAudio() calls
    std::vector<float> inputBuf48_;     // 48 kHz input samples not yet processed
    std::vector<float> processedBuf48_; // processed 48 kHz samples not yet downsampled
    std::vector<float> tempFrame48_;    // scratch buffer - one RNNoise frame (480 samples)
    std::vector<float> floatIn_;        // raw → float conversion scratch
    std::vector<float> floatOut_;       // float → raw conversion scratch
};
