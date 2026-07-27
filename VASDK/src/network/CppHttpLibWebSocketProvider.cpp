#include <VASDK/network/providers/CppHttpLibWebSocketProvider.h>
#include <spdlog/spdlog.h>

namespace va {
namespace network {
namespace providers {

bool CppHttpLibWebSocketProvider::listen(uint16_t port, const std::string& host) {
    spdlog::error("[CppHttpLibWebSocketProvider] WebSocket server not supported: "
                  "cpp-httplib does not implement a WebSocket server. "
                  "Inject a concrete WebSocketInterface into WebSocketTransport "
                  "(e.g. based on websocketpp or boost::beast). "
                  "Attempted bind: {}:{}", host, port);
    NetworkError err;
    err.code    = NetworkErrorCode::LibraryError;
    err.message = "WebSocket server not implemented in this build";
    if (onError) onError(err, 0);
    return false;
}

void CppHttpLibWebSocketProvider::stop() {}

} // namespace providers
} // namespace network
} // namespace va
