#include "SDLAudioSink.h"
#include <spdlog/spdlog.h>

SDLAudioSink::SDLAudioSink() {}

SDLAudioSink::~SDLAudioSink() {}

bool SDLAudioSink::isAudioSinkReady() const { return _isReady; }

bool SDLAudioSink::initializeAudioSink(const va::audio::AudioConfig &audioConfig) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        spdlog::error("failed to initialize the audio subsystem: {}", SDL_GetError());
        return false;
    }

    int deviceCount = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&deviceCount);
    if (deviceCount == 0 || devices == nullptr) {
        spdlog::error("No playback devices available!");
        return false;
    }

    _audioDeviceId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    //_audioDeviceId = devices[deviceCount-1]; // Use the first available device
    SDL_free(devices);

    spdlog::info("There are {} audio playback devices available, using device "
                 "with id: {}",
                 deviceCount,
                 _audioDeviceId);

    SDL_AudioSpec desiredSpec;
    SDL_zero(desiredSpec);
    desiredSpec.freq = audioConfig.sampleRate;

    switch (audioConfig.format) {
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
        releaseAudioSink();
        return false;
    }

    desiredSpec.channels = audioConfig.channels;

    if (!SDL_OpenAudioDevice(_audioDeviceId, &desiredSpec)) {
        spdlog::error("could not open the playback device: {}", SDL_GetError());
        releaseAudioSink();
        return false;
    }

    _audioStream = SDL_OpenAudioDeviceStream(_audioDeviceId, &desiredSpec, nullptr, nullptr);

    if (!_audioStream) {
        spdlog::error("failed to open playback stream: {}", SDL_GetError());
        return false;
    }

    if (!SDL_GetAudioStreamFormat(_audioStream, &desiredSpec, &desiredSpec)) {
        spdlog::error("failed to get stream format: {}", SDL_GetError());
        return false;
    }

    _audioConfig = audioConfig;

    _isReady = true;

    spdlog::info("audio playback device opened successfully, id: {}", _audioDeviceId);

    return true;
}

void SDLAudioSink::releaseAudioSink() {
    if (isAudioSinkReady()) {
        SDL_DestroyAudioStream(_audioStream);
        _audioStream = nullptr;
        _isReady = false;
        SDL_CloseAudioDevice(_audioDeviceId);
        spdlog::info("audio playback device released: {}", _audioDeviceId);
        _audioDeviceId = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int SDLAudioSink::writeAudio(const char *buffer, int offset, int length) {
    if (!isAudioSinkReady()) {
        spdlog::error("playback device not ready");
        return -1;
    }
    if (!isAudioSinkStarted()) {
        spdlog::warn("playback device not started");
        return -1;
    }

    const int sampleSize = getAudioSinkConfig().getSampleSizeInBytes();

    // Diff enqueued-byte count before/after the write to get actual bytes written.
    const int preWriteNumBytes = SDL_GetAudioStreamAvailable(_audioStream);

    if (!SDL_PutAudioStreamData(_audioStream, buffer + offset * sampleSize, length * sampleSize)) {
        spdlog::error("failed to write data to the playback device: {}", SDL_GetError());
        return -1;
    }

    const int postWriteNumBytes = SDL_GetAudioStreamAvailable(_audioStream);
    const int bytesWritten = postWriteNumBytes - preWriteNumBytes;

    if (bytesWritten > 0) {
        std::lock_guard<std::mutex> lk(_callbackMutex);
        if (_playbackCallback)
            _playbackCallback(buffer + offset * sampleSize, static_cast<size_t>(bytesWritten));
    }

    return (sampleSize > 0 && bytesWritten >= 0) ? (bytesWritten / sampleSize) : length;
}

void SDLAudioSink::setPlaybackCallback(PlaybackCallback cb) {
    std::lock_guard<std::mutex> lk(_callbackMutex);
    _playbackCallback = std::move(cb);
}

bool SDLAudioSink::startAudioSink() {
    if (isAudioSinkStarted())
        return true;

    if (!isAudioSinkReady()) {
        spdlog::error("playback device not ready.");
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(_audioStream)) {
        spdlog::error("could not resume playback device: {}", SDL_GetError());
        return false;
    }

    _isStarted = true;

    spdlog::info("audio playback device started: {}", _audioDeviceId);

    return true;
}

void SDLAudioSink::stopAudioSink() {
    if (!isAudioSinkStarted()) {
        spdlog::warn("playback device not started yet.");
        return;
    }

    if (!isAudioSinkReady()) {
        spdlog::warn("playback device not ready yet.");
        return;
    }

    if (!SDL_PauseAudioStreamDevice(_audioStream)) {
        spdlog::error("could not pause playback device: {}", SDL_GetError());
        return;
    }

    spdlog::info("audio playback device stopped: {}", _audioDeviceId);

    _isStarted = false;
}

bool SDLAudioSink::isAudioSinkStarted() const { return isAudioSinkReady() && _isStarted; }

int SDLAudioSink::getPendingAudioBytes() const {
    if (!_audioStream) return 0;
    return static_cast<int>(SDL_GetAudioStreamAvailable(_audioStream));
}

void SDLAudioSink::flushAudio() {
    if (_audioStream) SDL_ClearAudioStream(_audioStream);
}
