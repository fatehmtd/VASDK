#include "FileTransport.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <vector>

//  Lifecycle 

FileTransport::FileTransport(const std::string& filePath)
    : filePath_(filePath)
    , _audioSource(std::make_shared<FFmpegAudioSource>(filePath))
{
    // Default: 16 kHz, mono, 16-bit PCM (matches LocalTransport defaults).
    audioConfig_.sampleRate = 16000;
    audioConfig_.channels   = 1;
    audioConfig_.format     = va::audio::AudioFormat::SHORT;
}

FileTransport::~FileTransport() {
    stop();
    release();
}

bool FileTransport::initialize() {
    if (!_audioSource->initializeAudioSource(audioConfig_)) {
        spdlog::error("[FileTransport] Failed to initialize audio source for: {}", filePath_);
        return false;
    }

    setState(va::transport::TransportState::DISCONNECTED);
    spdlog::info("[FileTransport] Initialized: {}", filePath_);
    return true;
}

bool FileTransport::start() {
    if (getState() == va::transport::TransportState::CONNECTED) {
        spdlog::warn("[FileTransport] Already started");
        return true;
    }

    setState(va::transport::TransportState::CONNECTING);

    if (!_audioSource->startAudioSource()) {
        spdlog::error("[FileTransport] Failed to start audio source");
        setState(va::transport::TransportState::ERROR);
        return false;
    }

    _captureRunning = true;
    _captureThread  = std::thread(&FileTransport::captureThreadFunc, this);

    setState(va::transport::TransportState::CONNECTED);
    fireOnConnected();
    spdlog::info("[FileTransport] Started");
    return true;
}

void FileTransport::stop() {
    {
        // Guard against redundant stop() calls.
        const auto s = getState();
        if (s == va::transport::TransportState::DISCONNECTED ||
            s == va::transport::TransportState::DISCONNECTING) {
            return;
        }
    }

    setState(va::transport::TransportState::DISCONNECTING);
    _captureRunning = false;

    if (_captureThread.joinable()) {
        if (std::this_thread::get_id() == _captureThread.get_id()) {
            // stop() was called from within the capture thread itself
            // (e.g., from the onEndOfFile callback).  Detach to avoid
            // a self-join deadlock; the thread is already unwinding.
            _captureThread.detach();
        } else {
            _captureThread.join();
        }
    }

    if (_audioSource) {
        _audioSource->stopAudioSource();
    }

    setState(va::transport::TransportState::DISCONNECTED);
    fireOnDisconnected();
    spdlog::info("[FileTransport] Stopped");
}

void FileTransport::release() {
    stop();
    if (_audioSource) {
        _audioSource->releaseAudioSource();
    }
    spdlog::info("[FileTransport] Released");
}

bool FileTransport::sendFrame(std::shared_ptr<va::transport::Frame> frame) {
    // FileTransport is input-only; silently discard output frames.
    spdlog::debug("[FileTransport] sendFrame ignored (input-only transport), type={}",
        frame ? static_cast<int>(frame->getType()) : -1);
    return true;
}

//  Capture thread 

void FileTransport::captureThreadFunc() {
    spdlog::info("[FileTransport] Capture thread started (frameSizeInSamples={})",
        _frameSizeInSamples);

    const size_t bufSize  = _frameSizeInSamples * audioConfig_.getSampleSizeInBytes();
    std::vector<char> buf(bufSize);

    // Pace delivery to approximate real-time playback speed, which prevents
    // flooding the downstream pipeline with frames faster than it can process them.
    const int sleepMs = static_cast<int>(
        _frameSizeInSamples * 1000 / audioConfig_.sampleRate);

    while (_captureRunning) {
        const int samplesRead = _audioSource->readAudio(
            buf.data(), 0, static_cast<int>(_frameSizeInSamples));

        if (samplesRead < 0) {
            spdlog::error("[FileTransport] Error reading audio from source");
            incrementErrorCount();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            continue;
        }

        if (samplesRead == 0) {
            // End of file.
            spdlog::info("[FileTransport] End of file reached: {}", filePath_);
            _captureRunning = false;

            // enqueue a special control frame to signal EOF to the pipeline.
            enqueueReceivedFrame(std::make_shared<va::transport::AudioSourceEndControlFrame>()); 

            // Fire the user callback.  The callback may call stop(), which is
            // safe because stop() detects the self-join situation (see stop()).
            if (onEndOfFile) {
                onEndOfFile();
            }
            break;
        }

        auto frame = bufferToFrame(buf.data(), static_cast<uint32_t>(samplesRead));
        enqueueReceivedFrame(frame);
        updateReceivedStats(static_cast<size_t>(samplesRead) * audioConfig_.getSampleSizeInBytes());

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    spdlog::info("[FileTransport] Capture thread stopped");
}

std::shared_ptr<va::transport::AudioFrame>
FileTransport::bufferToFrame(const char* buffer, uint32_t numSamples) {
    auto frame = std::make_shared<va::transport::AudioFrame>();

    unsigned long ringbufferOffset = 0;
    unsigned long postWriteOffset  = 0;

    if (_ringBuffer) {
        ringbufferOffset = static_cast<unsigned long>(_ringBuffer->getOffset());
        postWriteOffset  = static_cast<unsigned long>(
            _ringBuffer->write(buffer, 0, static_cast<int>(numSamples)));
    }

    frame->numSamples        = numSamples;
    frame->sampleRate        = static_cast<uint32_t>(audioConfig_.sampleRate);
    frame->channels          = static_cast<uint32_t>(audioConfig_.channels);
    frame->sampleSizeInBytes = static_cast<uint32_t>(audioConfig_.getSampleSizeInBytes());
    frame->format            = static_cast<int>(audioConfig_.format);
    frame->absoluteOffset    = _absoluteOffset;
    frame->timestamp         = va::transport::Frame::getCurrentTimestamp();
    frame->sequenceId        = _sequenceCounter++;
    frame->frameId           = "file_" + std::to_string(frame->sequenceId);
    frame->sourceId          = "FileTransport";
    frame->ringbufferOffset  = ringbufferOffset;
    frame->postWriteOffset   = postWriteOffset;

    _absoluteOffset += numSamples;
    return frame;
}
