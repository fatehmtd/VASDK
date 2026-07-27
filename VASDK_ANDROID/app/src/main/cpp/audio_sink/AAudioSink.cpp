#include "AAudioSink.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cstring>

namespace va {
namespace android {

AAudioSink::AAudioSink() {
    _fifo.resize(kFifoCapacity);
}

AAudioSink::~AAudioSink() {
    releaseAudioSink();
}

bool AAudioSink::isAudioSinkReady()    const { return _ready.load(); }
bool AAudioSink::isAudioSinkStarted()  const { return _started.load(); }

bool AAudioSink::initializeAudioSink(const va::audio::AudioConfig& audioConfig) {
    spdlog::info("[AAudioSink] Initializing ({}Hz, {}ch)...",
                 audioConfig.sampleRate, audioConfig.channels);

    aaudio_format_t fmt;
    switch (audioConfig.format) {
    case va::audio::SHORT: fmt = AAUDIO_FORMAT_PCM_I16;   break;
    case va::audio::FLOAT: fmt = AAUDIO_FORMAT_PCM_FLOAT; break;
    default:
        spdlog::error("[AAudioSink] Unsupported audio format");
        return false;
    }

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        spdlog::error("[AAudioSink] Failed to create stream builder");
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, audioConfig.sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, audioConfig.channels);
    AAudioStreamBuilder_setFormat(builder, fmt);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, &AAudioSink::dataCallback, this);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &_stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        spdlog::error("[AAudioSink] Failed to open stream: {}",
                      AAudio_convertResultToText(result));
        return false;
    }

    _audioConfig   = audioConfig;
    _bytesPerFrame = audioConfig.getSampleSizeInBytes();

    {
        std::lock_guard<std::mutex> lock(_fifoMutex);
        _fifoReadPos = _fifoWritePos = _fifoSize = 0;
    }

    _ready = true;
    spdlog::info("[AAudioSink] Initialized.");
    return true;
}

void AAudioSink::releaseAudioSink() {
    if (!_ready) return;
    stopAudioSink();
    if (_stream) {
        AAudioStream_close(_stream);
        _stream = nullptr;
    }
    _ready = false;
    spdlog::info("[AAudioSink] Released.");
}

bool AAudioSink::startAudioSink() {
    if (_started) return true;
    if (!_ready) {
        spdlog::error("[AAudioSink] Not ready");
        return false;
    }
    aaudio_result_t result = AAudioStream_requestStart(_stream);
    if (result != AAUDIO_OK) {
        spdlog::error("[AAudioSink] Failed to start: {}",
                      AAudio_convertResultToText(result));
        return false;
    }
    _started = true;
    spdlog::info("[AAudioSink] Started.");
    return true;
}

void AAudioSink::stopAudioSink() {
    if (!_started) return;
    AAudioStream_requestStop(_stream);
    _started = false;
    spdlog::info("[AAudioSink] Stopped.");
}

int AAudioSink::writeAudio(const char* buffer, int offset, int length) {
    if (!_ready || !_started) return -1;

    const char* src   = buffer + static_cast<size_t>(offset) * _bytesPerFrame;
    size_t      bytes = static_cast<size_t>(length) * _bytesPerFrame;
    size_t      written = 0;

    // Back-pressure: block-and-retry until all bytes are queued so the
    // TTS thread is throttled to real-time playback speed.
    while (written < bytes && _started.load()) {
        std::lock_guard<std::mutex> lock(_fifoMutex);
        size_t available = kFifoCapacity - _fifoSize;
        if (available == 0) continue;

        size_t toWrite = std::min(bytes - written, available);
        const char* from = src + written;
        size_t seg1 = std::min(toWrite, kFifoCapacity - _fifoWritePos);
        std::memcpy(_fifo.data() + _fifoWritePos, from, seg1);
        if (toWrite > seg1)
            std::memcpy(_fifo.data(), from + seg1, toWrite - seg1);
        _fifoWritePos = (_fifoWritePos + toWrite) % kFifoCapacity;
        _fifoSize    += toWrite;
        written      += toWrite;
    }

    return static_cast<int>(written / _bytesPerFrame);
}

int AAudioSink::getPendingAudioBytes() const {
    std::lock_guard<std::mutex> lock(_fifoMutex);
    return static_cast<int>(_fifoSize);
}

void AAudioSink::flushAudio() {
    std::lock_guard<std::mutex> lock(_fifoMutex);
    _fifoReadPos = _fifoWritePos = _fifoSize = 0;
}

//  Static AAudio callback 

aaudio_data_callback_result_t AAudioSink::dataCallback(
    AAudioStream* /*stream*/, void* userData,
    void* audioData, int32_t numFrames)
{
    static_cast<AAudioSink*>(userData)->onPlayback(audioData, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioSink::onPlayback(void* audioData, int32_t numFrames) {
    char*  dst         = static_cast<char*>(audioData);
    size_t bytesNeeded = static_cast<size_t>(numFrames) * _bytesPerFrame;
    size_t bytesToRead;

    {
        std::lock_guard<std::mutex> lock(_fifoMutex);
        bytesToRead = std::min(bytesNeeded, _fifoSize);

        size_t seg1 = std::min(bytesToRead, kFifoCapacity - _fifoReadPos);
        std::memcpy(dst, _fifo.data() + _fifoReadPos, seg1);
        if (bytesToRead > seg1)
            std::memcpy(dst + seg1, _fifo.data(), bytesToRead - seg1);
        _fifoReadPos = (_fifoReadPos + bytesToRead) % kFifoCapacity;
        _fifoSize   -= bytesToRead;
    }

    // Fill remainder with silence to avoid crackle on underrun
    if (bytesToRead < bytesNeeded)
        std::memset(dst + bytesToRead, 0, bytesNeeded - bytesToRead);

    // Notify visualisation with the real (non-silence) bytes just played
    if (bytesToRead > 0) {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        if (_playbackCallback)
            _playbackCallback(dst, bytesToRead);
    }
}

void AAudioSink::setPlaybackCallback(PlaybackCallback cb) {
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _playbackCallback = std::move(cb);
}

} // namespace android
} // namespace va
