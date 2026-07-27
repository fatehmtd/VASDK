#pragma once
#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>
#include <VASDK/GenericAudioSink.h>
#include <mutex>

/**
 * @brief The SDLAudioSink class
 */
class SDLAudioSink : public va::audio::GenericAudioSink
{
private:
    /* data */
public:
    SDLAudioSink(/* args */);
    ~SDLAudioSink();

    // GenericAudioSink interface
public:
    bool isAudioSinkReady() const override;
    bool initializeAudioSink(const va::audio::AudioConfig &audioConfig) override;
    void releaseAudioSink() override;
    int writeAudio(const char *buffer, int offset, int length) override;
    bool startAudioSink() override;
    void stopAudioSink() override;
    bool isAudioSinkStarted() const override;
    int getPendingAudioBytes() const override;
    void flushAudio() override;
    void setPlaybackCallback(PlaybackCallback cb) override;

private:
    bool _isReady = false;
    bool _isStarted = false;
    SDL_AudioDeviceID _audioDeviceId;
    SDL_AudioStream *_audioStream = nullptr;

    PlaybackCallback _playbackCallback;
    std::mutex       _callbackMutex;
};
