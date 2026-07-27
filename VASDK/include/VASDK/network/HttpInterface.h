#pragma once

#include "NetworkError.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace va {
namespace network {

struct HttpResponse {
    int         statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    NetworkError error;
};

class HttpInterface {
public:
    virtual ~HttpInterface() = default;

    // Request/response control plane
    virtual HttpResponse post(const std::string& url,
                              const std::map<std::string, std::string>& headers,
                              const std::string& body,
                              int timeoutMs,
                              std::function<void(const std::string&)> onChunk = nullptr) = 0;

    virtual HttpResponse get(const std::string& url,
                             const std::map<std::string, std::string>& headers,
                             int timeoutMs) = 0;

    // Convenience data-plane wrappers - delegate to post() internally
    virtual bool sendText(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& text,
                          int timeoutMs) = 0;

    virtual bool sendBytes(const std::string& url,
                           const std::map<std::string, std::string>& headers,
                           const std::vector<uint8_t>& bytes,
                           int timeoutMs) = 0;

    // Receive callbacks - set by HttpTransport during initialize()
    std::function<void(const std::string&)>          onTextReceived;
    std::function<void(const std::vector<uint8_t>&)> onBytesReceived;
    std::function<void(const NetworkError&)>          onError;
};

} // namespace network
} // namespace va
