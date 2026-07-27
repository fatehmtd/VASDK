#pragma once

#include "CommonAudioUtils.h"
#include <functional>

namespace va {
namespace audio {
/**
 * @brief The GenericAudioSink class
 */
class GenericAudioSink {
public:
    /**
   * @brief whether the source is ready or not.
   * @return true if ready
   */
    virtual bool isAudioSinkReady() const = 0;

    /**
   * @brief initializes the audio sink properly
   * @param audioConfig the audio configuration
   * @return true if initialized, false otherwise
   */
    virtual bool initializeAudioSink(const AudioConfig &audioConfig) = 0;

    /**
   * @brief releases the audio sink
   */
    virtual void releaseAudioSink() = 0;

    /**
   * @brief write
   * @param buffer the buffer to write into
   * @param offset the offset within the buffer, in number of samples
   * @param length the length of the data to write, in number of samples
   * @return return the number of samples written, or -1 if an error occured
   */
    virtual int writeAudio(const char *buffer, int offset, int length) = 0;

    /**
     * @brief getAudioConfig returns the audio configuration
     * @return [AudioConfig]
     */
    AudioConfig getAudioSinkConfig() const { return _audioConfig; };

    /**
     * @brief Starts audio playback. Call after initialization.
     * @return true if started successfully, false otherwise
     */
    virtual bool startAudioSink() = 0;

    /**
     * @brief stops the audio sink
     * This method should be called to stop the audio sink gracefully.
     */
    virtual void stopAudioSink() = 0;

    /**
     * @brief isStarted
     * @return true if the audio sink is started, false otherwise
     */
    virtual bool isAudioSinkStarted() const = 0;

    /**
     * @brief Returns the number of bytes queued in the sink's output buffer
     *        but not yet consumed by the hardware.  Default returns 0.
     */
    virtual int getPendingAudioBytes() const { return 0; }

    /**
     * @brief Immediately discard all buffered audio not yet played.
     *        Used for barge-in / interrupt cancellation.
     */
    virtual void flushAudio() {}

    /**
     * @brief Apply a short linear ramp-down (fade-out) to buffered audio,
     *        then discard the remainder.  Prevents abrupt click on barge-in.
     *        Default falls back to instant flush.
     */
    virtual void fadeAndFlush() { flushAudio(); }

    /**
     * @brief Callback invoked each time a block of audio is actually sent to
     *        the hardware (non-silence bytes only).  Use for visualisation
     *        that must be in sync with real playback.
     *        Default implementation is a no-op; backends override as needed.
     */
    using PlaybackCallback = std::function<void(const char* data, size_t bytes)>;
    virtual void setPlaybackCallback(PlaybackCallback /*cb*/) {}

protected:
    AudioConfig _audioConfig;
};
} // namespace audio
} // namespace va
