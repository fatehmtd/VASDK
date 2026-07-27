#pragma once

#include "NetworkError.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace va {
namespace network {

class WebSocketInterface {
public:
    using ConnectionId = uint64_t;

    virtual ~WebSocketInterface() = default;

    // Control plane
    virtual bool   listen(uint16_t port, const std::string& host = "0.0.0.0") = 0;
    virtual void   stop() = 0;
    virtual size_t connectionCount() const = 0;

    // Data plane - per-connection or broadcast
    virtual bool sendText(ConnectionId id, const std::string& text) = 0;
    virtual bool sendBytes(ConnectionId id, const std::vector<uint8_t>& bytes) = 0;
    virtual void broadcastText(const std::string& text) = 0;
    virtual void broadcastBytes(const std::vector<uint8_t>& bytes) = 0;

    // Receive callbacks - set by WebSocketTransport during initialize()
    std::function<void(ConnectionId, const std::string&)>          onTextReceived;
    std::function<void(ConnectionId, const std::vector<uint8_t>&)> onBytesReceived;

    // Connection lifecycle callbacks
    std::function<void(ConnectionId)>               onConnect;
    std::function<void(ConnectionId)>               onDisconnect;
    // ConnectionId == 0 for server-level errors (e.g. listen failure)
    std::function<void(NetworkError, ConnectionId)> onError;
};

} // namespace network
} // namespace va
