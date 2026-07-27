#include "SDLAudioSource.h"
#include <limits>

SDLAudioSource::SDLAudioSource() : _audioDeviceId(0) {}

bool SDLAudioSource::isAudioSourceReady() const { return _isReady; }

bool SDLAudioSource::initializeAudioSource(const va::audio::AudioConfig &audioConfig)
{
    spdlog::info("Initializing SDL audio source...");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        spdlog::error("could not initialize the audio subsystem : {}", SDL_GetError());
        return false;
    }

    spdlog::info("SDL audio subsystem initialized.");

    int deviceCount = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&deviceCount);
    if (deviceCount == 0 || devices == nullptr)
    {
        spdlog::error("No recording devices available!");
        return false;
    }

    _audioDeviceId = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    //_audioDeviceId = devices[deviceCount-1]; // Use the last available device
    SDL_free(devices);

    spdlog::info("There are {} audio recording devices available, using device "
                 "with id: {}",
                 deviceCount,
                 _audioDeviceId);

    SDL_AudioSpec desiredSpec;
    SDL_zero(desiredSpec);
    desiredSpec.freq = audioConfig.sampleRate;

    switch (audioConfig.format)
    {
    case va::audio::BYTE:
        desiredSpec.format = SDL_AUDIO_S8;
        break;
    case va::audio::SHORT:
        desiredSpec.format = SDL_AUDIO_S16;
        break;
    case va::audio::INT:
        desiredSpec.format = SDL_AUDIO_S32;
        break;
    case va::audio::FLOAT:
        desiredSpec.format = SDL_AUDIO_F32;
        break;
    default:
        releaseAudioSource();
        return false;
    }

    desiredSpec.channels = audioConfig.channels;

    if (!SDL_OpenAudioDevice(_audioDeviceId, &desiredSpec))
    {
        spdlog::error("could not open the recording device: {}", SDL_GetError());
        releaseAudioSource();
        return false;
    }

    _audioStream = SDL_OpenAudioDeviceStream(_audioDeviceId, &desiredSpec, nullptr, nullptr);

    if (!_audioStream)
    {
        spdlog::error("failed to open recording stream: {}", SDL_GetError());
        return false;
    }

    if (!SDL_GetAudioStreamFormat(_audioStream, &desiredSpec, &desiredSpec))
    {
        spdlog::error("failed to get stream format: {}", SDL_GetError());
        return false;
    }

    _audioConfig = audioConfig;

    _isReady = true;

    spdlog::info("audio recording device opened successfully, id: {}", _audioDeviceId);

    return true;
}

void SDLAudioSource::releaseAudioSource()
{
    if (isAudioSourceReady())
    {
        SDL_DestroyAudioStream(_audioStream);
        _audioStream = nullptr;
        _isReady = false;
        SDL_CloseAudioDevice(_audioDeviceId);
        spdlog::info("audio recording device released: {}", _audioDeviceId);
        _audioDeviceId = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int SDLAudioSource::readAudio(char *buffer, int offset, int length)
{
    if (!isAudioSourceReady())
    {
        spdlog::error("recording device not ready.");
        return -1;
    }
    if (!isAudioSourceStarted())
    {
        spdlog::error("recording device not started.");
        return -1;
    }
    int sampleSize = getAudioSourceConfig().getSampleSizeInBytes();
    int bytesRead = SDL_GetAudioStreamData(_audioStream,
                                           buffer + sampleSize * offset,
                                           length * sampleSize);

    if (bytesRead < 0)
    {
        spdlog::error("failed to read from audio stream: {}", SDL_GetError());
        return -1;
    }

    int samplesRead = bytesRead / sampleSize;

    return samplesRead;
}

bool SDLAudioSource::startAudioSource()
{
    if (isAudioSourceStarted())
        return true;

    if (!isAudioSourceReady())
    {
        spdlog::error("device not ready.");
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(_audioStream))
    {
        spdlog::error("could not resume device: {}", SDL_GetError());
        return false;
    }

    _isStarted = true;

    spdlog::info("audio recording device started: {}", _audioDeviceId);

    return true;
}

void SDLAudioSource::stopAudioSource()
{
    if (!isAudioSourceStarted())
    {
        spdlog::warn("recording device not started yet.");
        return;
    }

    if (!isAudioSourceReady())
    {
        spdlog::warn("recording device not ready yet.");
        return;
    }

    if (!SDL_PauseAudioStreamDevice(_audioStream))
    {
        spdlog::error("could not pause recording device: {}", SDL_GetError());
        return;
    }

    spdlog::info("audio recording device stopped: {}", _audioDeviceId);

    _isStarted = false;
}

bool SDLAudioSource::isAudioSourceStarted() const { return _isStarted; }
