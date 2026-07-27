#pragma once
#include "CommonAudioUtils.h"
#include <vector>

namespace va {
namespace audio {
/**
 * @brief The VADProvider class
 */
class VADProvider {
public:
    struct VADResult {
        size_t offset; // in samples from the start of the buffer
        size_t length; // in samples
        float confidence; // confidence level of the detected speech activity, range [0.0, 1.0]
    };

    /**
   * @brief initialize
   * @param audioConfig
   * @param bufferSize
   * @return
   */
    virtual bool initialize(const AudioConfig &audioConfig, int bufferSize) = 0;

    /**
   * @brief release
   */
    virtual void release() = 0;

    /**
   * @brief detectVoiceActivity
   * @param buffer
   * @param offset
   * @param length
   * @return
   */
    virtual std::vector<VADResult> detectVoiceActivity(const char *buffer, size_t offset, size_t numSamples) const = 0;

    /**
   * @brief getAudioConfig
   * @return
   */
    AudioConfig getAudioConfig() const { return _audioConfig; }

protected:
    AudioConfig _audioConfig;
};
} // namespace audio
} // namespace va
