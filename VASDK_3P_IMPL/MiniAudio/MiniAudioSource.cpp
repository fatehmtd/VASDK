#include "MiniAudioSource.h"
#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

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

struct MiniAudioSource::Impl
{
    // size of the internal buffer
    size_t kRingCapacity = 1024 * 1024;

    ma_device           device{};
    ma_rb               rb{};
    bool                rbInitialized = false;
    bool                isReady       = false;
    bool                isStarted     = false;
    int                 bytesPerFrame = 1;

    // Lightweight signal so waitForData() wakes exactly when onCapture fires.
    // No audio data flows through this - it is just an event flag.
    std::atomic<bool>       hasData{false};
    std::condition_variable dataReady;
    std::mutex              dataReadyMutex;

    Impl(size_t internalBufferCapacity) : kRingCapacity(internalBufferCapacity) {}
    ~Impl()
    {
        if (rbInitialized) {
            ma_rb_uninit(&rb);
            rbInitialized = false;
        }
    }

    bool initRingBuffer()
    {
        if (ma_rb_init(kRingCapacity, nullptr, nullptr, &rb) != MA_SUCCESS)
            return false;
        rbInitialized = true;
        return true;
    }

    static void dataCallback(ma_device *pDevice, void * /*pOutput*/,
                             const void *pInput, ma_uint32 frameCount)
    {
        auto *impl = static_cast<Impl *>(pDevice->pUserData);
        if (impl && pInput)
            impl->onCapture(pInput, frameCount);
    }

    void onCapture(const void *pInput, ma_uint32 frameCount)
    {
        const char *src        = static_cast<const char *>(pInput);
        size_t      bytesLeft  = static_cast<size_t>(frameCount) * bytesPerFrame;

        // Lock-free SPSC write into ma_rb.
        // ma_rb_acquire_write gives a contiguous pointer; loop handles wrap-around.
        while (bytesLeft > 0) {
            void  *writePtr = nullptr;
            size_t chunk    = bytesLeft;
            if (ma_rb_acquire_write(&rb, &chunk, &writePtr) != MA_SUCCESS || chunk == 0)
                break; // ring full - drop the remaining bytes
            std::memcpy(writePtr, src, chunk);
            ma_rb_commit_write(&rb, chunk);
            src       += chunk;
            bytesLeft -= chunk;
        }

        hasData.store(true, std::memory_order_release);
        dataReady.notify_one();
    }
};

//  Construction / destruction 

MiniAudioSource::MiniAudioSource(size_t internalBufferCapacity) : _impl(std::make_unique<Impl>(internalBufferCapacity)) {}

MiniAudioSource::~MiniAudioSource()
{
    releaseAudioSource();
}

//  GenericAudioSource interface 

bool MiniAudioSource::isAudioSourceReady() const { return _impl->isReady; }

bool MiniAudioSource::initializeAudioSource(const va::audio::AudioConfig &audioConfig)
{
    spdlog::info("Initializing MiniAudio capture device...");

    ma_format fmt = toMaFormat(audioConfig.format);
    if (fmt == ma_format_unknown) {
        spdlog::error("MiniAudioSource: unsupported audio format.");
        return false;
    }

    if (!_impl->initRingBuffer()) {
        spdlog::error("MiniAudioSource: failed to initialize ring buffer.");
        return false;
    }

    ma_device_config cfg  = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format    = fmt;
    cfg.capture.channels  = static_cast<ma_uint32>(audioConfig.channels);
    cfg.sampleRate        = static_cast<ma_uint32>(audioConfig.sampleRate);
    cfg.dataCallback      = Impl::dataCallback;
    cfg.pUserData         = _impl.get();

    if (ma_device_init(nullptr, &cfg, &_impl->device) != MA_SUCCESS) {
        spdlog::error("MiniAudioSource: failed to initialize capture device.");
        ma_rb_uninit(&_impl->rb);
        _impl->rbInitialized = false;
        return false;
    }

    _audioConfig         = audioConfig;
    _impl->bytesPerFrame = audioConfig.getSampleSizeInBytes();
    _impl->isReady = true;
    spdlog::info("MiniAudio capture device initialized.");
    return true;
}

void MiniAudioSource::releaseAudioSource()
{
    if (!_impl->isReady)
        return;

    if (_impl->isStarted) {
        ma_device_stop(&_impl->device);
        _impl->isStarted = false;
    }
    ma_device_uninit(&_impl->device);
    _impl->isReady = false;
    spdlog::info("MiniAudio capture device released.");
}

int MiniAudioSource::readAudio(char *buffer, int offset, int length)
{
    if (!_impl->isReady) {
        spdlog::error("MiniAudioSource: device not ready.");
        return -1;
    }
    if (!_impl->isStarted) {
        spdlog::error("MiniAudioSource: device not started.");
        return -1;
    }

    char  *dst         = buffer + static_cast<size_t>(offset) * _impl->bytesPerFrame;
    size_t bytesNeeded = static_cast<size_t>(length) * _impl->bytesPerFrame;

    // Don't return a partial frame. MiniAudio's device callback fires every
    // ~10ms on Windows WASAPI, which is far smaller than the 125ms frame the
    // pipeline expects. Returning tiny chunks would produce VAD inputs below
    // the minimum window that Silero needs (~512 samples), resulting in no
    // voice detection. Callers loop with a 1ms yield when this returns 0,
    // so data accumulates lock-free until a full frame is ready.
    if (ma_rb_available_read(&_impl->rb) < static_cast<ma_uint32>(bytesNeeded))
        return 0;

    // Lock-free SPSC read from ma_rb; loop handles the wrap-around case
    // where a contiguous slice may span the end of the physical buffer.
    size_t bytesRead = 0;
    while (bytesRead < bytesNeeded) {
        void  *readPtr = nullptr;
        size_t chunk   = bytesNeeded - bytesRead;
        if (ma_rb_acquire_read(&_impl->rb, &chunk, &readPtr) != MA_SUCCESS || chunk == 0)
            break;
        std::memcpy(dst + bytesRead, readPtr, chunk);
        ma_rb_commit_read(&_impl->rb, chunk);
        bytesRead += chunk;
    }

    return static_cast<int>(bytesRead / _impl->bytesPerFrame);
}

bool MiniAudioSource::startAudioSource()
{
    if (_impl->isStarted)
        return true;
    if (!_impl->isReady) {
        spdlog::error("MiniAudioSource: device not ready.");
        return false;
    }
    if (ma_device_start(&_impl->device) != MA_SUCCESS) {
        spdlog::error("MiniAudioSource: failed to start capture device.");
        return false;
    }
    _impl->isStarted = true;
    spdlog::info("MiniAudio capture device started.");
    return true;
}

void MiniAudioSource::stopAudioSource()
{
    if (!_impl->isStarted)
        return;
    ma_device_stop(&_impl->device);
    _impl->isStarted = false;
    spdlog::info("MiniAudio capture device stopped.");
}

bool MiniAudioSource::isAudioSourceStarted() const { return _impl->isReady && _impl->isStarted; }

bool MiniAudioSource::waitForData(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_impl->dataReadyMutex);
    bool signalled = _impl->dataReady.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this] { return _impl->hasData.load(std::memory_order_acquire); });
    if (signalled) _impl->hasData.store(false, std::memory_order_release);
    return signalled;
}

} // namespace miniaudio
} // namespace va
