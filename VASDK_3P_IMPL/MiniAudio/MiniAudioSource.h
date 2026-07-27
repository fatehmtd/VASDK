#pragma once
#include <VASDK/GenericAudioSource.h>
#include <memory>
#include <chrono>

namespace va {
namespace miniaudio {

/**
 * @brief The MiniAudioSource class
 *
 * Capture (microphone) audio source backed by miniaudio.
 * Captured frames are pushed from miniaudio's callback thread into a
 * thread-safe circular FIFO; readAudio() drains that FIFO on the caller's
 * thread.
 */
class MiniAudioSource : public va::audio::GenericAudioSource
{
public:
    /**
     * @brief Construct a new MiniAudioSource object
     *
     * @param internalBufferCapacity  Size of the internal ring buffer in bytes.
     *                                Should be large enough to hold several seconds of audio to avoid overflow under load.
     */
    MiniAudioSource(size_t internalBufferCapacity);
    ~MiniAudioSource() override;

    // GenericAudioSource interface
    bool isAudioSourceReady() const override;
    bool initializeAudioSource(const va::audio::AudioConfig &audioConfig) override;
    void releaseAudioSource() override;
    int  readAudio(char *buffer, int offset, int length) override;
    bool startAudioSource() override;
    void stopAudioSource() override;
    bool isAudioSourceStarted() const override;

    /**
     * @brief Block until audio data is available or the timeout expires.
     *
     * This replaces a fixed sleep_for() in the capture loop - the thread
     * wakes exactly when the audio device delivers new data, eliminating
     * the jitter and dropped-frame accumulation caused by sleep overshoot
     * under CPU load.
     *
     * @param timeoutMs  Maximum time to wait in milliseconds.
     * @return true if data is available (readAudio() will return > 0 samples),
     *         false if the timeout elapsed before data arrived.
     */
    bool waitForData(int timeoutMs) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace miniaudio
} // namespace va
