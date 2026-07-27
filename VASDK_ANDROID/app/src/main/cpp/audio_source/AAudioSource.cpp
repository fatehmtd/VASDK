#include "AAudioSource.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace va {
namespace android {

AAudioSource::AAudioSource() {
    _fifo.resize(kFifoCapacity);
}

AAudioSource::~AAudioSource() {
    releaseAudioSource();
}

bool AAudioSource::isAudioSourceReady() const  { return _ready.load(); }
bool AAudioSource::isAudioSourceStarted() const { return _started.load(); }

bool AAudioSource::initializeAudioSource(const va::audio::AudioConfig& audioConfig) {
    spdlog::info("[AAudioSource] Initializing ({}Hz, {}ch)...",
                 audioConfig.sampleRate, audioConfig.channels);
    if (audioConfig.sampleRate != 16000) {
        spdlog::warn("[AAudioSource] Non-16kHz input configured ({} Hz). Parakeet is tuned for 16kHz; ensure resampling is enabled upstream.",
                     audioConfig.sampleRate);
    }
    if (audioConfig.channels != 1) {
        spdlog::warn("[AAudioSource] Non-mono input configured ({} channels). Multi-channel capture can degrade STT quality.",
                     audioConfig.channels);
    }

    switch (audioConfig.format) {
    case va::audio::SHORT: _aaudioFormat = AAUDIO_FORMAT_PCM_I16;   break;
    case va::audio::FLOAT: _aaudioFormat = AAUDIO_FORMAT_PCM_FLOAT; break;
    default:
        spdlog::error("[AAudioSource] Unsupported audio format");
        return false;
    }

    _audioConfig    = audioConfig;
    _bytesPerFrame  = audioConfig.getSampleSizeInBytes();

    {
        std::lock_guard<std::mutex> lock(_fifoMutex);
        _fifoReadPos = _fifoWritePos = _fifoSize = 0;
        _droppedBytes = 0;
        _overflowEvents = 0;
    }

    _ready = true;
    spdlog::info("[AAudioSource] Initialized.");
    return true;
}

void AAudioSource::releaseAudioSource() {
    if (!_ready) return;
    stopAudioSource();
    if (_stream) {
        AAudioStream_close(_stream);
        _stream = nullptr;
    }
    _ready = false;
    spdlog::info("[AAudioSource] Released.");
}

// Opening an INPUT stream is what triggers the RECORD_AUDIO permission check,
// so it is deferred until start: initializeAudioSource() runs during engine init,
// which may happen before the user has granted the microphone permission.
bool AAudioSource::openStream() {
    if (_stream) return true;

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        spdlog::error("[AAudioSource] Failed to create stream builder");
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, _audioConfig.sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, _audioConfig.channels);
    AAudioStreamBuilder_setFormat(builder, _aaudioFormat);
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, &AAudioSource::dataCallback, this);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &_stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        spdlog::error("[AAudioSource] Failed to open stream: {}",
                      AAudio_convertResultToText(result));
        _stream = nullptr;
        return false;
    }
    return true;
}

bool AAudioSource::startAudioSource() {
    if (_started) return true;
    if (!_ready) {
        spdlog::error("[AAudioSource] Not ready");
        return false;
    }
    if (!openStream()) return false;
    aaudio_result_t result = AAudioStream_requestStart(_stream);
    if (result != AAUDIO_OK) {
        spdlog::error("[AAudioSource] Failed to start: {}",
                      AAudio_convertResultToText(result));
        return false;
    }
    _started = true;
    spdlog::info("[AAudioSource] Started.");
    return true;
}

void AAudioSource::stopAudioSource() {
    if (!_started) return;
    AAudioStream_requestStop(_stream);
    _started = false;
    _dataReady.notify_all();
    spdlog::info("[AAudioSource] Stopped.");
}

int AAudioSource::readAudio(char* buffer, int offset, int length) {
    if (!_ready || !_started) return -1;

    char*  dst        = buffer + static_cast<size_t>(offset) * _bytesPerFrame;
    size_t bytesNeeded = static_cast<size_t>(length) * _bytesPerFrame;

    // Block until we have a full frame (length samples) or the deadline expires.
    // Using a deadline of 3× the expected frame duration prevents stalling forever
    // while still guaranteeing full-frame delivery to LocalTransport.
    // Without this accumulation, AAudio delivers one callback worth of data at a
    // time (~640 samples = 40 ms), causing the VAD to see only micro-bursts of
    // voice that never accumulate enough for the STARTING→ONGOING transition.
    const int frameMs = (length * 1000) / _audioConfig.sampleRate;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(frameMs * 3);

    size_t totalRead = 0;
    while (totalRead < bytesNeeded && _started.load()) {
        size_t chunk = 0;
        {
            std::unique_lock<std::mutex> lock(_fifoMutex);
            _dataReady.wait_until(lock, deadline, [this] {
                return _fifoSize > 0 || !_started.load();
            });
            chunk = std::min(bytesNeeded - totalRead, _fifoSize);
            if (chunk > 0) {
                size_t seg1 = std::min(chunk, kFifoCapacity - _fifoReadPos);
                std::memcpy(dst + totalRead, _fifo.data() + _fifoReadPos, seg1);
                if (chunk > seg1)
                    std::memcpy(dst + totalRead + seg1, _fifo.data(), chunk - seg1);
                _fifoReadPos = (_fifoReadPos + chunk) % kFifoCapacity;
                _fifoSize   -= chunk;
                totalRead   += chunk;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
    }

    return static_cast<int>(totalRead / _bytesPerFrame);
}

bool AAudioSource::waitForData(int timeoutMs) {
    std::unique_lock<std::mutex> lock(_fifoMutex);
    return _dataReady.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this] { return _fifoSize > 0 || !_started.load(); });
}

//  Static AAudio callback 

aaudio_data_callback_result_t AAudioSource::dataCallback(
    AAudioStream* /*stream*/, void* userData,
    void* audioData, int32_t numFrames)
{
    static_cast<AAudioSource*>(userData)->onCapture(audioData, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioSource::onCapture(const void* audioData, int32_t numFrames) {
    const char* src   = static_cast<const char*>(audioData);
    size_t      bytes = static_cast<size_t>(numFrames) * _bytesPerFrame;

    std::lock_guard<std::mutex> lock(_fifoMutex);

    // Drop oldest data on overflow rather than blocking the audio thread
    if (_fifoSize + bytes > kFifoCapacity) {
        size_t toDrop = (_fifoSize + bytes) - kFifoCapacity;
        _fifoReadPos  = (_fifoReadPos + toDrop) % kFifoCapacity;
        _fifoSize    -= toDrop;
        _droppedBytes += toDrop;
        ++_overflowEvents;
        if ((_overflowEvents % 25) == 1) {
            spdlog::warn("[AAudioSource] FIFO overflow: dropped {} bytes (events={}, totalDropped={} bytes)",
                         toDrop, _overflowEvents, _droppedBytes);
        }
    }

    size_t seg1 = std::min(bytes, kFifoCapacity - _fifoWritePos);
    std::memcpy(_fifo.data() + _fifoWritePos, src, seg1);
    if (bytes > seg1)
        std::memcpy(_fifo.data(), src + seg1, bytes - seg1);
    _fifoWritePos = (_fifoWritePos + bytes) % kFifoCapacity;
    _fifoSize    += bytes;
    _dataReady.notify_one();
}

} // namespace android
} // namespace va
