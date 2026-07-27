#pragma once

#include <string>

namespace va {
namespace network {

enum class NetworkErrorCode {
    None = 0,
    ConnectionFailed,  // could not reach host
    Timeout,           // operation timed out
    TlsError,          // SSL/TLS failure
    HttpError,         // 4xx / 5xx response - httpStatusCode is set
    ProtocolError,     // malformed / unexpected response
    SendFailed,        // write to socket failed
    ReceiveFailed,     // read from socket failed
    LibraryError,      // internal library error
};

struct NetworkError {
    NetworkErrorCode code          = NetworkErrorCode::None;
    std::string      message;
    int              httpStatusCode = 0;  // populated for HttpError
    int              nativeCode     = 0;  // raw CURLcode / httplib code for debugging

    bool ok() const { return code == NetworkErrorCode::None; }
    explicit operator bool() const { return !ok(); }
};

} // namespace network
} // namespace va
