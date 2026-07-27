#pragma once

#include "Transport.h"
#include <VASDK/network/WebSocketInterface.h>
#include <atomic>
#include <memory>

namespace va {
namespace transport {

/**
 * @brief WebSocket server transport - backed by a WebSocketInterface provider.
 *
 * Owns a WebSocketInterface (default: CppHttpLibWebSocketProvider).
 * Wires provider callbacks in initialize() to deserialize incoming frames and
 * track connection count. sendFrame() broadcasts to all connected clients.
 *
 * Inject a custom WebSocketInterface for testing or to swap the underlying
 * library without touching this class.
 */
class VASDK_EXPORT WebSocketTransport : public BaseTransport {
public:
    /**
     * @brief Construct WebSocketTransport
     * @param port     Port to listen on (default: 8080)
     * @param host     Host to bind to (default: "0.0.0.0")
     * @param wsAdapter Optional provider (defaults to CppHttpLibWebSocketProvider)
     */
    explicit WebSocketTransport(uint16_t port = 8080,
                                const std::string& host = "0.0.0.0",
                                std::shared_ptr<va::network::WebSocketInterface> wsAdapter = nullptr);

    ~WebSocketTransport() override;

    std::string getName() const override { return "WebSocketTransport"; }
    std::string getDescription() const override {
        return "WebSocket server transport for remote clients";
    }

    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    bool sendFrame(std::shared_ptr<Frame> frame) override;

    /**
     * @brief Get the number of currently connected clients
     */
    size_t getClientCount() const;

    void setMaxConnections(size_t maxConnections) { _maxConnections = maxConnections; }
    void setBinaryMode(bool binary) { _binaryMode = binary; }

protected:
    std::string serializeFrame(std::shared_ptr<Frame> frame);
    std::shared_ptr<Frame> deserializeFrame(const std::string& data);

private:
    uint16_t    _port;
    std::string _host;
    size_t      _maxConnections;
    bool        _binaryMode;

    std::shared_ptr<va::network::WebSocketInterface> _wsAdapter;
    std::atomic<bool>   _wsInitialized{false};
    std::atomic<size_t> _connectionCount{0};
    std::atomic<uint64_t> _sequenceCounter{0};
};

} // namespace transport
} // namespace va
