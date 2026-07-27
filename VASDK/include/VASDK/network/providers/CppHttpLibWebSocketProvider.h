#pragma once

#include <VASDK/network/WebSocketInterface.h>

namespace va {
namespace network {
namespace providers {

/**
 * @brief Placeholder WebSocket provider.
 *
 * cpp-httplib (v0.18.x) does not implement a WebSocket server.
 * This stub compiles cleanly and fails listen() at runtime with a
 * clear error so the rest of the architecture is preserved.
 *
 * To get a working WebSocket server, construct WebSocketTransport with a
 * concrete WebSocketInterface implementation (e.g. based on websocketpp
 * or boost::beast) injected via the constructor.
 */
class CppHttpLibWebSocketProvider : public WebSocketInterface {
public:
    CppHttpLibWebSocketProvider() = default;
    ~CppHttpLibWebSocketProvider() override = default;

    // Returns false - WebSocket server not available via cpp-httplib
    bool   listen(uint16_t port, const std::string& host = "0.0.0.0") override;
    void   stop() override;
    size_t connectionCount() const override { return 0; }

    bool sendText(ConnectionId /*id*/, const std::string& /*text*/) override { return false; }
    bool sendBytes(ConnectionId /*id*/, const std::vector<uint8_t>& /*bytes*/) override { return false; }
    void broadcastText(const std::string& /*text*/) override {}
    void broadcastBytes(const std::vector<uint8_t>& /*bytes*/) override {}
};

} // namespace providers
} // namespace network
} // namespace va
