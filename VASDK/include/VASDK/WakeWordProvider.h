#pragma once
#include "TypeName.h"
#include "CommonAudioUtils.h"

#include <optional>
#include <string>

namespace va {
namespace audio {

/**
 * @brief Listens for a wake phrase ("hey garage") in the mic audio.
 *
 * WakeWordProcessor feeds it every audio frame, in order. Implementations keep whatever
 * streaming state they need between calls.
 */
class WakeWordProvider {
public:
    struct Detection {
        std::string phrase;
        float confidence = 0.0f;
    };

    virtual ~WakeWordProvider() = default;
    virtual std::string getName() const { return shortTypeName(typeid(*this)); }  ///< for logs and UIs

    virtual bool initialize(const AudioConfig& audioConfig) = 0;
    /** `samples` holds `numSamples` samples in the initialized format. A detection, if this audio completed one. */
    virtual std::optional<Detection> processAudio(const char* samples, size_t numSamples) = 0;
    /** Forget partial matches, e.g. once the agent is already listening. */
    virtual void reset() {}
};

} // namespace audio
} // namespace va
