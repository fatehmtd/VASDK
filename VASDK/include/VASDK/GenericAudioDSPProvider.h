#pragma once

#include "CommonAudioUtils.h"
#include "vasdk_export.h"
#include <cstddef>
#include <string>

namespace va {
namespace audio {

/**
 * @brief Abstract interface for audio DSP (Digital Signal Processing) algorithms.
 *
 * GenericAudioDSPProvider defines a uniform contract for in-line audio
 * processing algorithms such as noise suppression, echo cancellation, or
 * dynamic-range compression.
 *
 * Audio is delivered as raw bytes in the format supplied to initialize() -
 * the implementation is responsible for any required sample-format conversion
 * and writing the result in the same format and size.
 *
 * Usage:
 * @code
 *   auto provider = std::make_shared<RNNoiseDSPProvider>();
 *   provider->initialize(audioConfig);
 *
 *   auto dspProcessor = std::make_shared<va::pipeline::AudioDSPProcessor>(provider);
 *   dspProcessor->setRawAudioRingBuffer(rawBuffer);
 *   dspProcessor->setCleanAudioRingBuffer(cleanBuffer);
 * @endcode
 */
class VASDK_EXPORT GenericAudioDSPProvider {
public:
    virtual ~GenericAudioDSPProvider() = default;

    /** @brief Human-readable name of the DSP algorithm (e.g. "RNNoise"). */
    virtual std::string getName() const = 0;

    /**
     * @brief Initialise the provider for the given audio configuration.
     * @param config  Audio format (sample rate, channels, sample format).
     * @return true on success, false on failure.
     */
    virtual bool initialize(const AudioConfig& config) = 0;

    /** @brief Release all resources. Must not be used again without initialize(). */
    virtual void release() = 0;

    /**
     * @brief Process one block of audio samples.
     *
     * @param input       Raw input audio (numSamples × getSampleSizeInBytes() bytes).
     * @param output      Buffer for the processed output (same size as input).
     * @param numSamples  Number of samples to process.
     * @return            Number of output samples written (should equal numSamples).
     */
    virtual size_t processAudio(const char* input, char* output,
                                size_t numSamples) = 0;

    /** @brief Return the audio configuration supplied to initialize(). */
    AudioConfig getAudioConfig() const { return _config; }

protected:
    AudioConfig _config{};
};

} // namespace audio
} // namespace va
