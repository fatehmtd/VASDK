#include "MiniAudioSink.h"
#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace va {
namespace miniaudio {

//  helpers (translation-unit local) 

namespace {
ma_format toMaFormat(va::audio::AudioFormat fmt)
{
    switch (fmt) {
    case va::audio::BYTE:  return ma_format_u8;
    case va::audio::SHORT: return ma_format_s16;
    case va::audio::INT:   return ma_format_s32;
    case va::audio::FLOAT: return ma_format_f32;
    default:               return ma_format_unknown;
    }
}
} // namespace

//  Impl 

struct MiniAudioSink::Impl
{
    static constexpr size_t kFifoCapacity = 8 * 1024 * 1024; // 8 MB - accommodates ~45s of 44100/16-bit/mono TTS

    ma_device           device{};
    bool                isReady       = false;
    std::atomic<bool>   isStarted     = false;
    int                 bytesPerFrame = 1;
    ma_format           format_       = ma_format_unknown;
    uint32_t            sampleRate_   = 44100;

    std::vector<char>   fifo;
    size_t              fifoReadPos   = 0;
    size_t              fifoWritePos  = 0;
    size_t              fifoSize      = 0;
    mutable std::mutex  fifoMutex;

    va::audio::GenericAudioSink::PlaybackCallback playbackCallback;
    std::mutex                      callbackMutex;

    Impl() { fifo.resize(kFifoCapacity); }

    static void dataCallback(ma_device *pDevice, void *pOutput,
                             const void * /*pInput*/, ma_uint32 frameCount)
    {
        auto *impl = static_cast<Impl *>(pDevice->pUserData);
        if (impl && pOutput)
            impl->onPlayback(pOutput, frameCount);
    }

    void onPlayback(void *pOutput, ma_uint32 frameCount)
    {
        char  *dst         = static_cast<char *>(pOutput);
        size_t bytesNeeded = static_cast<size_t>(frameCount) * bytesPerFrame;

        size_t bytesToRead;
        {
            std::lock_guard<std::mutex> lock(fifoMutex);
            bytesToRead = std::min(bytesNeeded, fifoSize);

            size_t seg1 = std::min(bytesToRead, kFifoCapacity - fifoReadPos);
            std::memcpy(dst, fifo.data() + fifoReadPos, seg1);
            if (bytesToRead > seg1)
                std::memcpy(dst + seg1, fifo.data(), bytesToRead - seg1);
            fifoReadPos = (fifoReadPos + bytesToRead) % kFifoCapacity;
            fifoSize -= bytesToRead;
        }

        // Fill remaining output with silence to avoid noise on underrun
        if (bytesToRead < bytesNeeded)
            std::memset(dst + bytesToRead, 0, bytesNeeded - bytesToRead);

        // Notify visualisation with the real (non-silence) bytes just played
        if (bytesToRead > 0) {
            std::lock_guard<std::mutex> lock(callbackMutex);
            if (playbackCallback)
                playbackCallback(dst, bytesToRead);
        }
    }
};

//  Construction / destruction 

MiniAudioSink::MiniAudioSink() : _impl(std::make_unique<Impl>()) {}

MiniAudioSink::~MiniAudioSink()
{
    releaseAudioSink();
}

//  GenericAudioSink interface 

bool MiniAudioSink::isAudioSinkReady() const { return _impl->isReady; }

bool MiniAudioSink::initializeAudioSink(const va::audio::AudioConfig &audioConfig)
{
    spdlog::info("Initializing MiniAudio playback device...");

    ma_format fmt = toMaFormat(audioConfig.format);
    if (fmt == ma_format_unknown) {
        spdlog::error("MiniAudioSink: unsupported audio format.");
        return false;
    }

    ma_device_config cfg  = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = fmt;
    cfg.playback.channels = static_cast<ma_uint32>(audioConfig.channels);
    cfg.sampleRate        = static_cast<ma_uint32>(audioConfig.sampleRate);
    cfg.dataCallback      = Impl::dataCallback;
    cfg.pUserData         = _impl.get();

    if (ma_device_init(nullptr, &cfg, &_impl->device) != MA_SUCCESS) {
        spdlog::error("MiniAudioSink: failed to initialize playback device.");
        return false;
    }

    _audioConfig          = audioConfig;
    _impl->bytesPerFrame  = audioConfig.getSampleSizeInBytes();
    _impl->format_        = fmt;
    _impl->sampleRate_    = static_cast<uint32_t>(audioConfig.sampleRate);

    {
        std::lock_guard<std::mutex> lock(_impl->fifoMutex);
        _impl->fifoReadPos  = 0;
        _impl->fifoWritePos = 0;
        _impl->fifoSize     = 0;
    }

    _impl->isReady = true;
    spdlog::info("MiniAudio playback device initialized.");
    return true;
}

void MiniAudioSink::releaseAudioSink()
{
    if (!_impl->isReady)
        return;

    if (_impl->isStarted) {
        ma_device_stop(&_impl->device);
        _impl->isStarted = false;
    }
    ma_device_uninit(&_impl->device);
    _impl->isReady = false;
    spdlog::info("MiniAudio playback device released.");
}

int MiniAudioSink::writeAudio(const char *buffer, int offset, int length)
{
    if (!_impl->isReady) {
        spdlog::error("MiniAudioSink: device not ready.");
        return -1;
    }
    if (!_impl->isStarted) {
        spdlog::warn("MiniAudioSink: device not started.");
        return -1;
    }

    const char *src    = buffer + static_cast<size_t>(offset) * _impl->bytesPerFrame;
    size_t      bytes  = static_cast<size_t>(length) * _impl->bytesPerFrame;
    size_t      written = 0;

    // Block-and-retry instead of dropping: back-pressure naturally throttles
    // the TTS thread to real-time playback speed when the FIFO fills up.
    while (written < bytes && _impl->isStarted) {
        size_t toWrite = 0;
        {
            std::lock_guard<std::mutex> lock(_impl->fifoMutex);
            size_t available = Impl::kFifoCapacity - _impl->fifoSize;
            if (available > 0) {
                toWrite = std::min(bytes - written, available);
                const char *fromPtr = src + written;
                size_t seg1 = std::min(toWrite, Impl::kFifoCapacity - _impl->fifoWritePos);
                std::memcpy(_impl->fifo.data() + _impl->fifoWritePos, fromPtr, seg1);
                if (toWrite > seg1)
                    std::memcpy(_impl->fifo.data(), fromPtr + seg1, toWrite - seg1);
                _impl->fifoWritePos = (_impl->fifoWritePos + toWrite) % Impl::kFifoCapacity;
                _impl->fifoSize += toWrite;
                written += toWrite;
            }
        }
        if (toWrite == 0) {
            // FIFO still full - yield and retry after playback has drained some
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    return static_cast<int>(written / _impl->bytesPerFrame);
}

bool MiniAudioSink::startAudioSink()
{
    if (_impl->isStarted)
        return true;
    if (!_impl->isReady) {
        spdlog::error("MiniAudioSink: device not ready.");
        return false;
    }
    if (ma_device_start(&_impl->device) != MA_SUCCESS) {
        spdlog::error("MiniAudioSink: failed to start playback device.");
        return false;
    }
    _impl->isStarted = true;
    spdlog::info("MiniAudio playback device started.");
    return true;
}

void MiniAudioSink::stopAudioSink()
{
    if (!_impl->isStarted)
        return;
    ma_device_stop(&_impl->device);
    _impl->isStarted = false;
    spdlog::info("MiniAudio playback device stopped.");
}

bool MiniAudioSink::isAudioSinkStarted() const { return _impl->isReady && _impl->isStarted; }

int MiniAudioSink::getPendingAudioBytes() const
{
    std::lock_guard<std::mutex> lock(_impl->fifoMutex);
    return static_cast<int>(_impl->fifoSize);
}

void MiniAudioSink::flushAudio()
{
    std::lock_guard<std::mutex> lock(_impl->fifoMutex);
    _impl->fifoReadPos  = 0;
    _impl->fifoWritePos = 0;
    _impl->fifoSize     = 0;
}

void MiniAudioSink::fadeAndFlush()
{
    // Apply a ~50 ms linear ramp-down in-place to the buffered samples,
    // then drop the remainder.  This prevents a hard click on barge-in.
    std::lock_guard<std::mutex> lock(_impl->fifoMutex);

    const size_t kFifoCapacity = Impl::kFifoCapacity;
    const int    bytesPerFrame = _impl->bytesPerFrame;

    // Number of fade samples: 50 ms worth, capped to what's actually buffered.
    const int fadeSamples = static_cast<int>(_impl->sampleRate_ * 50u / 1000u);
    const int available   = static_cast<int>(_impl->fifoSize / static_cast<size_t>(bytesPerFrame));
    const int n           = std::min(fadeSamples, available);

    if (n <= 0) {
        // Nothing buffered - just clear.
        _impl->fifoReadPos  = 0;
        _impl->fifoWritePos = 0;
        _impl->fifoSize     = 0;
        return;
    }

    // Apply gain ramp: starts at 1.0 and reaches 0.0 at the last sample.
    if (_impl->format_ == ma_format_s16) {
        for (int i = 0; i < n; ++i) {
            float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
            size_t bytePos = (_impl->fifoReadPos + static_cast<size_t>(i) * 2u) % kFifoCapacity;
            auto* s = reinterpret_cast<int16_t*>(_impl->fifo.data() + bytePos);
            *s = static_cast<int16_t>(static_cast<float>(*s) * gain);
        }
    } else if (_impl->format_ == ma_format_f32) {
        for (int i = 0; i < n; ++i) {
            float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
            size_t bytePos = (_impl->fifoReadPos + static_cast<size_t>(i) * 4u) % kFifoCapacity;
            auto* s = reinterpret_cast<float*>(_impl->fifo.data() + bytePos);
            *s *= gain;
        }
    }
    // (For other formats fall through: truncate without fade below.)

    // Retain only the faded region; drop the rest.
    const size_t fadedBytes  = static_cast<size_t>(n) * static_cast<size_t>(bytesPerFrame);
    _impl->fifoWritePos = (_impl->fifoReadPos + fadedBytes) % kFifoCapacity;
    _impl->fifoSize     = fadedBytes;
}

void MiniAudioSink::setPlaybackCallback(va::audio::GenericAudioSink::PlaybackCallback cb)
{
    std::lock_guard<std::mutex> lock(_impl->callbackMutex);
    _impl->playbackCallback = std::move(cb);
}

} // namespace miniaudio
} // namespace va
