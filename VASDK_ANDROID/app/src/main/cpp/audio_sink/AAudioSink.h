#pragma once

#include <VASDK/GenericAudioSink.h>
#include <VASDK/PlaybackHoldEnvelope.h>
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace va {
namespace android {

/**
 * @brief AAudio-backed speaker sink for Android.
 *
 * Audio is written into a thread-safe circular FIFO by writeAudio();
 * the AAudio playback callback drains that FIFO on the audio thread.
 */
class AAudioSink : public va::audio::GenericAudioSink {
public:
    AAudioSink();
    virtual ~AAudioSink();

    bool isAudioSinkReady()  const override;
    bool initializeAudioSink(const va::audio::AudioConfig& audioConfig) override;
    void releaseAudioSink() override;
    int  writeAudio(const char* buffer, int offset, int length) override;
    bool startAudioSink()  override;
    void stopAudioSink()   override;
    bool isAudioSinkStarted() const override;
    int  getPendingAudioBytes() const override;
    void flushAudio() override;
    void holdPlayback(va::transport::PendingPlaybackMode pendingPlaybackMode, bool held,
                      float duckGainDecibels) override;
    void setPlaybackCallback(PlaybackCallback cb) override;

private:
    static aaudio_data_callback_result_t dataCallback(
        AAudioStream* stream, void* userData,
        void* audioData, int32_t numFrames);

    void onPlayback(void* audioData, int32_t numFrames);

    AAudioStream*     _stream{nullptr};
    std::atomic<bool> _ready{false};
    std::atomic<bool> _started{false};
    int               _bytesPerFrame{1};
    va::audio::PlaybackHoldEnvelope _playbackHoldEnvelope;  // applied on the AAudio callback thread

    static constexpr size_t kFifoCapacity = 8 * 1024 * 1024; // 8 MB
    std::vector<char>       _fifo;
    size_t                  _fifoReadPos{0};
    size_t                  _fifoWritePos{0};
    size_t                  _fifoSize{0};
    mutable std::mutex      _fifoMutex;

    PlaybackCallback   _playbackCallback;
    mutable std::mutex _callbackMutex;
};

} // namespace android
} // namespace va
