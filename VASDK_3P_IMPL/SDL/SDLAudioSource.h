#pragma once
#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>
#include <VASDK/GenericAudioSource.h>

/**
 * @brief The SDLAudioSource class
 */
class SDLAudioSource : public va::audio::GenericAudioSource
{
public:
    SDLAudioSource();

    // GenericAudioSource interface
public:
    bool isAudioSourceReady() const override;
    bool initializeAudioSource(const va::audio::AudioConfig &audioConfig) override;
    void releaseAudioSource() override;
    int readAudio(char *buffer, int offset, int length) override;
    bool startAudioSource() override;
    void stopAudioSource() override;
    bool isAudioSourceStarted() const override;

private:
    bool _isReady = false;
    bool _isStarted = false;
    SDL_AudioDeviceID _audioDeviceId;
    SDL_AudioStream *_audioStream = nullptr;
};
