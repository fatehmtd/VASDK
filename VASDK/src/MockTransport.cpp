#include "VASDK/MockTransport.h"
#include <spdlog/spdlog.h>
#include <thread>

namespace va {
namespace transport {

MockTransport::MockTransport()
    : _autoResponse(false)
    , _simulatedLatencyMs(0)
{
}

MockTransport::~MockTransport() {
    stop();
    release();
}

bool MockTransport::initialize() {
    setState(TransportState::DISCONNECTED);
    spdlog::info("[MockTransport] Initialized");
    return true;
}

bool MockTransport::start() {
    if (getState() == TransportState::CONNECTED) {
        spdlog::warn("[MockTransport] Already started");
        return true;
    }

    setState(TransportState::CONNECTING);

    if (_simulatedLatencyMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_simulatedLatencyMs));
    }

    setState(TransportState::CONNECTED);
    spdlog::info("[MockTransport] Started");
    return true;
}

void MockTransport::stop() {
    if (getState() == TransportState::DISCONNECTED) {
        return;
    }

    setState(TransportState::DISCONNECTING);
    setState(TransportState::DISCONNECTED);
    spdlog::info("[MockTransport] Stopped");
}

void MockTransport::release() {
    stop();
    clearSentFrames();
    spdlog::info("[MockTransport] Released");
}

bool MockTransport::sendFrame(std::shared_ptr<Frame> frame) {
    if (!isConnected()) {
        spdlog::warn("[MockTransport] Cannot send frame: not connected");
        return false;
    }

    if (!frame) {
        spdlog::warn("[MockTransport] Cannot send null frame");
        return false;
    }

    if (_simulatedLatencyMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_simulatedLatencyMs));
    }

    // Store sent frame for verification (getSentFrames / getSentFramesByType)
    {
        std::lock_guard<std::mutex> lock(_sentFramesMutex);
        _sentFrames.push_back(frame->clone());
    }

    size_t estimatedSize = 1024; // Rough estimate
    if (frame->getType() == FrameType::AUDIO) {
        auto audioFrame = std::dynamic_pointer_cast<AudioFrame>(frame);
        if (audioFrame) {
            estimatedSize = audioFrame->getSizeInBytes();
        }
    }
    updateSentStats(estimatedSize);

    if (_autoResponse) {
        handleAutoResponse(frame);
    }

    spdlog::debug("[MockTransport] Sent frame: type={}, id={}",
        static_cast<int>(frame->getType()), frame->frameId);

    return true;
}

void MockTransport::injectFrame(std::shared_ptr<Frame> frame) {
    if (!frame) {
        spdlog::warn("[MockTransport] Cannot inject null frame");
        return;
    }

    if (_simulatedLatencyMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_simulatedLatencyMs));
    }

    enqueueReceivedFrame(frame);

    size_t estimatedSize = 1024;
    if (frame->getType() == FrameType::AUDIO) {
        auto audioFrame = std::dynamic_pointer_cast<AudioFrame>(frame);
        if (audioFrame) {
            estimatedSize = audioFrame->getSizeInBytes();
        }
    }
    updateReceivedStats(estimatedSize);

    spdlog::debug("[MockTransport] Injected frame: type={}, id={}",
        static_cast<int>(frame->getType()), frame->frameId);
}

std::vector<std::shared_ptr<Frame>> MockTransport::getSentFrames() const {
    std::lock_guard<std::mutex> lock(_sentFramesMutex);
    return _sentFrames;
}

std::vector<std::shared_ptr<Frame>> MockTransport::getSentFramesByType(FrameType type) const {
    std::lock_guard<std::mutex> lock(_sentFramesMutex);
    std::vector<std::shared_ptr<Frame>> filtered;
    for (const auto& frame : _sentFrames) {
        if (frame->getType() == type) {
            filtered.push_back(frame);
        }
    }
    return filtered;
}

void MockTransport::clearSentFrames() {
    std::lock_guard<std::mutex> lock(_sentFramesMutex);
    _sentFrames.clear();
}

size_t MockTransport::getSentFrameCount() const {
    std::lock_guard<std::mutex> lock(_sentFramesMutex);
    return _sentFrames.size();
}

void MockTransport::simulateError(int errorCode, const std::string& errorMessage) {
    incrementErrorCount();
    fireOnError(errorCode, errorMessage);
    spdlog::warn("[MockTransport] Simulated error: code={}, msg={}", errorCode, errorMessage);
}

void MockTransport::simulateDisconnection() {
    if (isConnected()) {
        setState(TransportState::DISCONNECTED);
        spdlog::info("[MockTransport] Simulated disconnection");
    }
}

void MockTransport::handleAutoResponse(std::shared_ptr<Frame> frame) {
    switch (frame->getType()) {
        case FrameType::AUDIO: {
            auto transcriptionFrame = std::make_shared<TranscriptionFrame>();
            transcriptionFrame->transcriptionResult.text = "Mock transcription response";
            transcriptionFrame->isFinal = true;
            transcriptionFrame->transcriptionResult.confidence = 0.95f;
            transcriptionFrame->timestamp = Frame::getCurrentTimestamp();
            transcriptionFrame->frameId = "mock_transcription_" + std::to_string(transcriptionFrame->timestamp);
            transcriptionFrame->sourceId = "MockTransport";

            std::thread([this, transcriptionFrame]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                injectFrame(transcriptionFrame);
            }).detach();
            break;
        }

        case FrameType::TEXT: {
            auto synthesisFrame = std::make_shared<SynthesisFrame>();
            auto textFrame = std::dynamic_pointer_cast<TextFrame>(frame);
            if (textFrame) {
                synthesisFrame->text = textFrame->text;
            }
            synthesisFrame->audioData.resize(16000); // 1 second at 16kHz, silence
            synthesisFrame->timestamp = Frame::getCurrentTimestamp();
            synthesisFrame->frameId = "mock_synthesis_" + std::to_string(synthesisFrame->timestamp);
            synthesisFrame->sourceId = "MockTransport";

            std::thread([this, synthesisFrame]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                injectFrame(synthesisFrame);
            }).detach();
            break;
        }

        default:
            break;
    }
}

} // namespace transport
} // namespace va
