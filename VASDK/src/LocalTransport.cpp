#include "VASDK/LocalTransport.h"
#include <spdlog/spdlog.h>

namespace va {
namespace transport {

LocalTransport::LocalTransport(
    std::shared_ptr<audio::GenericAudioSource> audioSource,
    std::shared_ptr<audio::GenericAudioSink> audioSink
)
    : _audioSource(audioSource)
    , _audioSink(audioSink)
    , _captureRunning(false)
    , _frameSizeInSamples(2000)  // Default: 125ms at 16kHz
    , _absoluteOffset(0)
    , _sequenceCounter(0)
{
    // Default audio config: 16kHz, mono, 16-bit PCM
    _inputAudioConfig.sampleRate = 16000;
    _inputAudioConfig.channels = 1;
    _inputAudioConfig.format = audio::AudioFormat::SHORT;

    _outputAudioConfig.sampleRate = 16000;
    _outputAudioConfig.channels = 1;
    _outputAudioConfig.format = audio::AudioFormat::SHORT;
}

LocalTransport::~LocalTransport() {
    stop();
    release();
}

bool LocalTransport::initialize() {
    if (!_audioSource || !_audioSink) {
        spdlog::error("[LocalTransport] Audio source or sink is null");
        return false;
    }

    if (!_audioSource->initializeAudioSource(_inputAudioConfig)) {
        spdlog::error("[LocalTransport] Failed to initialize audio source");
        return false;
    }

    if (!_audioSink->initializeAudioSink(_outputAudioConfig)) {
        spdlog::error("[LocalTransport] Failed to initialize audio sink");
        _audioSource->releaseAudioSource();
        return false;
    }

    setState(TransportState::DISCONNECTED);
    spdlog::info("[LocalTransport] Initialized successfully");

    // Register metrics
    if (_metricsConfig.enabled && _collector) {
        using namespace va::metrics;
        _callbackJitterSampler.initialize(_collector,
            MetricDescriptor{"localtransport_capture_jitter_ms", MetricType::Sampler,
                             "Audio capture callback interval jitter (ms)",
                             _metricsConfig.jitterWindow});
        _underrunCounter.initialize(_collector,
            MetricDescriptor{"localtransport_underruns_total", MetricType::Counter,
                             "Audio capture underrun count"});
    }

    return true;
}

bool LocalTransport::start() {
    if (getState() == TransportState::CONNECTED) {
        spdlog::warn("[LocalTransport] Already started");
        return true;
    }

    setState(TransportState::CONNECTING);

    if (!_audioSource->startAudioSource()) {
        spdlog::error("[LocalTransport] Failed to start audio source");
        setState(TransportState::ERROR);
        return false;
    }

    if (!_audioSink->startAudioSink()) {
        spdlog::error("[LocalTransport] Failed to start audio sink");
        _audioSource->stopAudioSource();
        setState(TransportState::ERROR);
        return false;
    }

    _captureRunning = true;
    _captureThread = std::thread(&LocalTransport::captureThreadFunc, this);

    setState(TransportState::CONNECTED);
    spdlog::info("[LocalTransport] Started successfully");
    return true;
}

void LocalTransport::stop() {
    if (getState() == TransportState::DISCONNECTED) {
        return;
    }

    setState(TransportState::DISCONNECTING);

    _captureRunning = false;
    if (_captureThread.joinable()) {
        _captureThread.join();
    }

    if (_audioSource) {
        _audioSource->stopAudioSource();
    }
    if (_audioSink) {
        _audioSink->stopAudioSink();
    }

    setState(TransportState::DISCONNECTED);
    spdlog::info("[LocalTransport] Stopped");
}

void LocalTransport::release() {
    stop();

    if (_audioSource) {
        _audioSource->releaseAudioSource();
    }
    if (_audioSink) {
        _audioSink->releaseAudioSink();
    }

    spdlog::info("[LocalTransport] Released");
}

bool LocalTransport::sendFrame(std::shared_ptr<Frame> frame) {
    if (!isConnected()) {
        spdlog::warn("[LocalTransport] Cannot send frame: not connected");
        return false;
    }

    if (!frame) {
        spdlog::warn("[LocalTransport] Cannot send null frame");
        return false;
    }

    // Handle SynthesisFrame output (TTS audio)
    if (frame->getType() == FrameType::SYNTHESIS) {
        auto synthesisFrame = std::dynamic_pointer_cast<SynthesisFrame>(frame);
        if (!synthesisFrame) {
            spdlog::error("[LocalTransport] Failed to cast to SynthesisFrame");
            return false;
        }

        if (synthesisFrame->audioData.empty()) {
            spdlog::warn("[LocalTransport] SynthesisFrame has no audio data");
            return true;
        }

        // audioData is a char vector; convert byte count to sample count for writeAudio
        int numSamples = synthesisFrame->audioData.size() / _outputAudioConfig.getSampleSizeInBytes();
        int samplesWritten = _audioSink->writeAudio(
            synthesisFrame->audioData.data(),
            0,
            numSamples
        );

        if (samplesWritten < 0) {
            spdlog::error("[LocalTransport] Failed to write synthesis audio to sink");
            incrementErrorCount();
            return false;
        }

        updateSentStats(synthesisFrame->audioData.size());
        return true;
    }

    // Ignore other frame types (could log or handle differently)
    spdlog::debug("[LocalTransport] Ignoring non-audio frame type {} for output", 
                  static_cast<int>(frame->getType()));
    return true;
}

void LocalTransport::captureThreadFunc() {
    spdlog::info("[LocalTransport] Capture thread started (frameSizeInSamples={})", _frameSizeInSamples);

    size_t bufferSize = _frameSizeInSamples * _inputAudioConfig.getSampleSizeInBytes();
    std::vector<char> rawBuffer(bufferSize);

    int sleepDurationMs = (_frameSizeInSamples * 1000) / _inputAudioConfig.sampleRate; // Used as waitForData timeout and error backoff
    _firstCapture = true;

    while (_captureRunning) {
        // Block until the audio device delivers new data. For sources backed by a
        // condition variable (e.g. MiniAudio) this wakes exactly when the device
        // callback fires, eliminating the blind sleep that caused audio to pile up
        // in the FIFO and introduced growing latency. For sources whose base-class
        // waitForData returns true immediately (e.g. SDL) the empty-read path below
        // handles the pacing with a short yield.
        _audioSource->waitForData(sleepDurationMs);

        auto captureNow = std::chrono::steady_clock::now();
        if (!_firstCapture) {
            double intervalMs = std::chrono::duration<double, std::milli>(
                captureNow - _lastCaptureTime).count();
            // Jitter = deviation from expected device period
            double jitterMs = std::abs(intervalMs - static_cast<double>(sleepDurationMs));
            _callbackJitterSampler.observe(jitterMs);
        }
        _lastCaptureTime = captureNow;
        _firstCapture = false;

        int samplesRead = _audioSource->readAudio(
            rawBuffer.data(),
            0,
            _frameSizeInSamples
        );

        if (samplesRead < 0) {
            spdlog::error("[LocalTransport] Error reading audio: {}", samplesRead);
            incrementErrorCount();
            _underrunCounter.increment();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepDurationMs));
            continue;
        }

        if (samplesRead == 0) {
            // waitForData returned but the source had nothing (e.g. SDL stream race).
            // Yield briefly to avoid a busy-spin; waitForData at the top handles pacing.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        
        // bufferToFrame also writes the raw audio into the ring buffer
        auto frame = bufferToFrame(rawBuffer.data(), static_cast<uint32_t>(samplesRead));

        enqueueReceivedFrame(frame);

        size_t frameSize = samplesRead * _inputAudioConfig.getSampleSizeInBytes();
        updateReceivedStats(frameSize);

        // No sleep here - waitForData at the top of the loop handles pacing,
        // waking precisely when the next device callback delivers audio.
    }

    spdlog::info("[LocalTransport] Capture thread stopped");
}

std::shared_ptr<AudioFrame> LocalTransport::bufferToFrame(const char*buffer, uint32_t numSamples) {
    auto frame = std::make_shared<AudioFrame>();

    unsigned long ringbufferOffset = 0;
    unsigned long postWriteOffset = 0;

    if (_ringBuffer) {
        ringbufferOffset = _ringBuffer->getOffset();
        postWriteOffset = _ringBuffer->write(buffer, 0, numSamples);
    } else {
        spdlog::debug("[LocalTransport] No ring buffer configured, skipping write");
    }

    // Create lightweight frame (no samples data, just metadata)
    frame->numSamples = static_cast<uint32_t>(numSamples);
    frame->sampleRate = _inputAudioConfig.sampleRate;
    frame->channels = _inputAudioConfig.channels;
    frame->sampleSizeInBytes = _inputAudioConfig.getSampleSizeInBytes();
    frame->format = static_cast<int>(_inputAudioConfig.format);
    frame->absoluteOffset = _absoluteOffset;
    frame->timestamp = Frame::getCurrentTimestamp();
    frame->sequenceId = _sequenceCounter++;
    frame->frameId = "_audio" + std::to_string(frame->sequenceId);
    frame->sourceId = "LocalTransport";
    frame->ringbufferOffset = ringbufferOffset;
    frame->postWriteOffset = postWriteOffset;

    // Update absolute offset (in samples, not bytes)
    _absoluteOffset += numSamples;

    return frame;
}

} // namespace transport
} // namespace va
