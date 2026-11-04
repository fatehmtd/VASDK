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

    /** Follow redirects (default true). Off for callers that check the URL they reach. */
    void setFollowRedirects(bool follow) { _options.followRedirects = follow; }
    /** Check the server's certificate (default false: embedded Android builds often lack CA certificates). */
    void setVerifyTls(bool verify) { _options.verifyTls = verify; }
    /** A longer reply fails with ReceiveFailed; 0 (default): no limit. */
    void setMaxResponseBytes(size_t bytes) { _options.maxResponseBytes = bytes; }

    struct Options {
        bool followRedirects = true;
        bool verifyTls = false;
        size_t maxResponseBytes = 0;
    };

private:
    Options _options;
    // Each request records the generation it started in and aborts once it changes.
    std::atomic<uint64_t> _cancelGeneration{0};
};

} // namespace providers
} // namespace network
} // namespace va
