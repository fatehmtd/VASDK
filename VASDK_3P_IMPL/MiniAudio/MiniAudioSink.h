#pragma once
#include <VASDK/GenericAudioSink.h>
#include <memory>

namespace va {
namespace miniaudio {

/**
 * @brief The MiniAudioSink class
 *
 * Playback audio sink backed by miniaudio.
 * writeAudio() pushes samples into a thread-safe circular FIFO; miniaudio's
 * callback thread drains it, filling with silence when the FIFO runs dry.
 */
class MiniAudioSink : public va::audio::GenericAudioSink
{
public:
    MiniAudioSink();
    ~MiniAudioSink();

    // GenericAudioSink interface
    bool isAudioSinkReady() const override;
    bool initializeAudioSink(const va::audio::AudioConfig &audioConfig) override;
    void releaseAudioSink() override;
    int  writeAudio(const char *buffer, int offset, int length) override;
    bool startAudioSink() override;
    void stopAudioSink() override;
    bool isAudioSinkStarted() const override;
    int  getPendingAudioBytes() const override;
    void flushAudio() override;
    void fadeAndFlush() override;

    void setPlaybackCallback(PlaybackCallback cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace miniaudio
} // namespace va
