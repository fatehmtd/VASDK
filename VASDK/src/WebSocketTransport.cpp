#include "VASDK/WebSocketTransport.h"
#include <VASDK/network/providers/CppHttpLibWebSocketProvider.h>
#include <spdlog/spdlog.h>

// Undefine Windows ERROR macro to avoid conflict with TransportState::ERROR
#ifdef ERROR
#undef ERROR
#endif

#ifdef VASDK_HAS_JSON
    #include <nlohmann/json.hpp>
#endif

namespace va {
namespace transport {

WebSocketTransport::WebSocketTransport(uint16_t port, const std::string& host,
                                       std::shared_ptr<va::network::WebSocketInterface> wsAdapter)
    : _port(port), _host(host), _maxConnections(10), _binaryMode(true)
    , _wsAdapter(std::move(wsAdapter))
{
}

WebSocketTransport::~WebSocketTransport() {
    stop();
    release();
}

bool WebSocketTransport::initialize() {
    if (_wsInitialized) return true;

    if (!_wsAdapter)
        _wsAdapter = std::make_shared<va::network::providers::CppHttpLibWebSocketProvider>();

    using ConnectionId = va::network::WebSocketInterface::ConnectionId;

    _wsAdapter->onConnect = [this](ConnectionId id) {
        if (_connectionCount.fetch_add(1) == 0) {
            setState(TransportState::CONNECTED);
            fireOnConnected();
        }
        spdlog::debug("[WebSocketTransport] Client {} connected ({} total)",
                      id, _connectionCount.load());
    };

    _wsAdapter->onDisconnect = [this](ConnectionId id) {
        size_t remaining = _connectionCount.fetch_sub(1) - 1;
        spdlog::debug("[WebSocketTransport] Client {} disconnected ({} remaining)", id, remaining);
        if (remaining == 0) {
            setState(TransportState::DISCONNECTED);
            fireOnDisconnected();
        }
    };

    _wsAdapter->onTextReceived = [this](ConnectionId /*id*/, const std::string& text) {
        auto frame = deserializeFrame(text);
        if (frame) enqueueReceivedFrame(frame);
    };

    _wsAdapter->onBytesReceived = [this](ConnectionId /*id*/,
                                         const std::vector<uint8_t>& bytes) {
        auto frame = deserializeFrame(std::string(bytes.begin(), bytes.end()));
        if (frame) enqueueReceivedFrame(frame);
    };

    _wsAdapter->onError = [this](const va::network::NetworkError& err, ConnectionId /*id*/) {
        spdlog::error("[WebSocketTransport] Error {}: {}",
                      static_cast<int>(err.code), err.message);
        setState(TransportState::ERROR);

        auto errFrame = std::make_shared<ErrorFrame>();
        errFrame->errorCode    = err.nativeCode ? err.nativeCode : err.httpStatusCode;
        errFrame->errorMessage = err.message;
        errFrame->severity     = ErrorSeverity::ERROR;
        enqueueReceivedFrame(errFrame);
        fireOnError(errFrame->errorCode, err.message);
    };

    setState(TransportState::DISCONNECTED);
    _wsInitialized = true;
    spdlog::info("[WebSocketTransport] Initialized for {}:{}", _host, _port);
    return true;
}

bool WebSocketTransport::start() {
    if (!_wsInitialized && !initialize()) return false;
    if (getState() == TransportState::CONNECTED) return true;

    setState(TransportState::CONNECTING);
    if (!_wsAdapter->listen(_port, _host)) {
        setState(TransportState::ERROR);
        fireOnError(-1, "WebSocket server failed to start on " + _host + ":" + std::to_string(_port));
        return false;
    }

    spdlog::info("[WebSocketTransport] Listening on {}:{}", _host, _port);
    return true;
}

void WebSocketTransport::stop() {
    if (!_wsInitialized || !_wsAdapter) return;
    _wsAdapter->stop();
    _connectionCount = 0;
    setState(TransportState::DISCONNECTED);
    spdlog::info("[WebSocketTransport] Stopped");
}

void WebSocketTransport::release() {
    stop();
    _wsInitialized = false;
}

bool WebSocketTransport::sendFrame(std::shared_ptr<Frame> frame) {
    if (!frame || !_wsAdapter || _wsAdapter->connectionCount() == 0) return false;

    std::string payload = serializeFrame(frame);
    if (payload.empty()) return false;

    if (_binaryMode)
        _wsAdapter->broadcastBytes(std::vector<uint8_t>(payload.begin(), payload.end()));
    else
        _wsAdapter->broadcastText(payload);

    updateSentStats(payload.size());
    return true;
}

size_t WebSocketTransport::getClientCount() const {
    if (!_wsAdapter) return 0;
    return _wsAdapter->connectionCount();
}

std::string WebSocketTransport::serializeFrame(std::shared_ptr<Frame> frame) {
#ifdef VASDK_HAS_JSON
    try {
        nlohmann::json j;
        j["type"]       = static_cast<int>(frame->getType());
        j["timestamp"]  = frame->timestamp;
        j["sequenceId"] = frame->sequenceId;
        j["frameId"]    = frame->frameId;
        j["sourceId"]   = frame->sourceId;

        if (frame->getType() == FrameType::AUDIO) {
            auto audioFrame = std::dynamic_pointer_cast<AudioFrame>(frame);
            if (audioFrame) {
                j["sampleRate"]     = audioFrame->sampleRate;
                j["channels"]       = audioFrame->channels;
                j["format"]         = audioFrame->format;
                j["numSamples"]     = audioFrame->numSamples;
                j["absoluteOffset"] = audioFrame->absoluteOffset;
            }
        }
        return j.dump();
    } catch (const std::exception& e) {
        spdlog::error("[WebSocketTransport] Serialization error: {}", e.what());
        return "";
    }
#else
    spdlog::warn("[WebSocketTransport] JSON not available - stub serialization");
    return "{}";
#endif
}

std::shared_ptr<Frame> WebSocketTransport::deserializeFrame(const std::string& data) {
#ifdef VASDK_HAS_JSON
    try {
        auto j = nlohmann::json::parse(data);
        auto frameType = static_cast<FrameType>(j["type"].get<int>());

        if (frameType == FrameType::AUDIO) {
            auto frame = std::make_shared<AudioFrame>();
            frame->timestamp      = j["timestamp"].get<uint64_t>();
            frame->sequenceId     = j["sequenceId"].get<uint64_t>();
            frame->frameId        = j["frameId"].get<std::string>();
            frame->sourceId       = j["sourceId"].get<std::string>();
            frame->sampleRate     = j["sampleRate"].get<uint32_t>();
            frame->channels       = j["channels"].get<uint32_t>();
            frame->format         = j["format"].get<int>();
            frame->numSamples     = j["numSamples"].get<uint32_t>();
            frame->absoluteOffset = j["absoluteOffset"].get<uint64_t>();
            return frame;
        }
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::error("[WebSocketTransport] Deserialization error: {}", e.what());
        return nullptr;
    }
#else
    spdlog::warn("[WebSocketTransport] JSON not available - stub deserialization");
    return nullptr;
#endif
}

} // namespace transport
} // namespace va
