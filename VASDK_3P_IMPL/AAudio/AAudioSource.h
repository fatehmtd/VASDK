#pragma once

#include <VASDK/GenericAudioSource.h>
#include <aaudio/AAudio.h>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace va::thirdparty::audio {

/**
 * @brief AAudio-backed microphone source for Android.
 *
 * Captured audio is pushed from the AAudio callback thread into a
 * thread-safe circular FIFO; readAudio() drains that FIFO on the
 * pipeline capture thread.
 */
/** Settings of AAudioSource. */
struct AAudioSourceOptions {
    /** The FIFO between the device callback and the pipeline, in bytes (at least 4096). */
    size_t bufferBytes = 1024 * 1024;
};

class AAudioSource : public va::audio::GenericAudioSource {
public:
    explicit AAudioSource(AAudioSourceOptions options = {});
    virtual ~AAudioSource();

    bool isAudioSourceReady()  const override;
    bool initializeAudioSource(const va::audio::AudioConfig& audioConfig) override;
    void releaseAudioSource() override;
    int  readAudio(char* buffer, int offset, int length) override;
    bool startAudioSource() override;
    void stopAudioSource()  override;
    bool isAudioSourceStarted() const override;
    bool waitForData(int timeoutMs) override;

private:
    static aaudio_data_callback_result_t dataCallback(
        AAudioStream* stream, void* userData,
        void* audioData, int32_t numFrames);

    void onCapture(const void* audioData, int32_t numFrames);

    bool openStream();

    AAudioStream*      _stream{nullptr};
    std::atomic<bool>  _ready{false};
    std::atomic<bool>  _started{false};
    int                _bytesPerFrame{1};
    aaudio_format_t    _aaudioFormat{AAUDIO_FORMAT_PCM_I16};
    va::audio::AudioConfig _audioConfig{};

    size_t _fifoCapacity;
    std::vector<char>       _fifo;
    size_t                  _fifoReadPos{0};
    size_t                  _fifoWritePos{0};
    size_t                  _fifoSize{0};
    uint64_t                _droppedBytes{0};
    uint64_t                _overflowEvents{0};
    mutable std::mutex      _fifoMutex;
    std::condition_variable _dataReady;
};

} // namespace va::thirdparty::audio
