#pragma once
#include "CommonAudioUtils.h"
#include <cstddef>

namespace va {
namespace audio {
/**
 * @brief The GenericAudioSource class
 */
class GenericAudioSource {
public:
    virtual ~GenericAudioSource() = default;

    /**
     * @brief whether the source is ready or not.
     * @return true if ready
     */
    virtual bool isAudioSourceReady() const = 0;

    /**
     * @brief initializes the source
     * @return true if initialized properly
     */
    virtual bool initializeAudioSource(const AudioConfig &audioConfig) = 0;

    /**
     * @brief releases the audio source
     */
    virtual void releaseAudioSource() = 0;

    /**
     * @brief getAvailableAudioSamples returns the number of audio samples that are currently available to read
     * @return the number of available audio samples or -1 if streaming
     */
    virtual size_t getAvailableAudioSamples() const { return -1; };

    /**
     * @brief reads data into the buffer
     * @param buffer the buffer to read into
     * @param offset the offset within the buffer, in number of samples
     * @param length the length of data to be read, in number of samples
     * @return the number of samples read or -1 if an error occured.
     */
    virtual int readAudio(char *buffer, int offset, int length) = 0;

    /**
     * @brief getAudioConfig returns the audio configuration
     * @return [AudioConfig]
     */
    AudioConfig getAudioSourceConfig() const { return _audioConfig; };

    /**
     * @brief start
     * @return
     */
    virtual bool startAudioSource() = 0;

    /**
     * @brief stop
     */
    virtual void stopAudioSource() = 0;

    /**
     * @brief isStarted
     * @return
     */
    virtual bool isAudioSourceStarted() const = 0;

    /**
     * @brief Block until audio data is available or the timeout expires.
     *
     * The default implementation returns true immediately, preserving
     * compatibility with sources that use polling or synchronous reads.
     * Override in sources that can efficiently block (e.g. via a
     * condition_variable) to eliminate fixed-duration sleeps in the
     * capture thread and avoid dropped frames under CPU load.
     *
     * @param timeoutMs  Maximum time to wait in milliseconds.
     * @return true if data is (likely) available, false on timeout.
     */
    virtual bool waitForData(int /*timeoutMs*/) { return true; }
protected:
    AudioConfig _audioConfig;
};
} // namespace audio
} // namespace va
