#pragma once

#include "CommonAudioUtils.h"
#include <cstddef>
#include <vector>

namespace va {
namespace audio {

/**
 * @brief Abstract interface for speaker embedding providers.
 *
 * Turns one utterance into a fixed-size voice print. SpeakerIdentificationProcessor
 * owns enrollment and matching (cosine similarity between voice prints), so a provider
 * only has to wrap the embedding model (e.g. the pyannote / WeSpeaker ResNet34 model
 * in VASDK_3P_IMPL/PyannoteSpeaker).
 */
class SpeakerEmbeddingProvider {
public:
    virtual ~SpeakerEmbeddingProvider() = default;

    /**
     * @brief Compute the speaker embedding of an utterance.
     * @param samples    Mono samples in [-1, 1] at getAudioConfig().sampleRate.
     * @param numSamples Number of samples.
     * @return The embedding, or an empty vector on failure.
     */
    virtual std::vector<float> computeEmbedding(const float* samples, size_t numSamples) = 0;

    /**
     * @brief Format of the pipeline audio the provider was initialized with.
     */
    AudioConfig getAudioConfig() const { return _audioConfig; }

protected:
    AudioConfig _audioConfig{};
};

} // namespace audio
} // namespace va
