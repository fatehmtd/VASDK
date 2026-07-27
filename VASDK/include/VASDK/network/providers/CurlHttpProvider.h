#pragma once

#include <VASDK/network/HttpInterface.h>

namespace va {
namespace network {
namespace providers {

/**
 * @brief libcurl implementation of HttpInterface.
 *
 * All libcurl symbols are confined to CurlHttpProvider.cpp behind
 * VASDK_HAS_CURL. Callers depend only on HttpInterface.
 */
class CurlHttpProvider : public HttpInterface {
public:
    CurlHttpProvider() = default;
    ~CurlHttpProvider() override = default;

    HttpResponse post(const std::string& url,
                      const std::map<std::string, std::string>& headers,
                      const std::string& body,
                      int timeoutMs,
                      std::function<void(const std::string&)> onChunk = nullptr) override;

    HttpResponse get(const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     int timeoutMs) override;

    bool sendText(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& text,
                  int timeoutMs) override;

    bool sendBytes(const std::string& url,
                   const std::map<std::string, std::string>& headers,
                   const std::vector<uint8_t>& bytes,
                   int timeoutMs) override;
};

} // namespace providers
} // namespace network
} // namespace va
