#pragma once

#include <VASDK/network/HttpInterface.h>
#include <atomic>
#include <cstdint>

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

    void cancel() override { _cancelGeneration.fetch_add(1); }

private:
    // Each request records the generation it started in and aborts once it changes.
    std::atomic<uint64_t> _cancelGeneration{0};
};

} // namespace providers
} // namespace network
} // namespace va
