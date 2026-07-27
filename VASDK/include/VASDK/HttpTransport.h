#pragma once

#include "Transport.h"
#include <VASDK/network/HttpInterface.h>
#include <memory>

namespace va {
namespace transport {

/**
 * @brief HTTP network transport.
 *
 * Owns an HttpInterface and wires its callbacks during initialize().
 * Exposes protected httpPost() / httpGet() helpers and virtual
 * onHttpText() / onHttpBytes() / onHttpError() hooks for subclasses.
 *
 * LLMTransport extends this class and overrides the hooks to add
 * LLM-specific payload building, conversation history, and SSE parsing.
 * HttpTransport itself knows nothing about frame types or LLM logic.
 */
class VASDK_EXPORT HttpTransport : public BaseTransport {
public:
    explicit HttpTransport(std::shared_ptr<va::network::HttpInterface> client = nullptr);
    ~HttpTransport() override;

    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    bool isConnected() const override;

    // Default sendFrame is a no-op - subclasses override to build payloads
    bool sendFrame(std::shared_ptr<Frame> frame) override { return false; }

    std::string getName() const override { return "HttpTransport"; }
    std::string getDescription() const override {
        return "HTTP network transport (libcurl or compatible provider)";
    }

protected:
    //  Network helpers for subclasses 

    va::network::HttpResponse httpPost(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& body,
        int timeoutMs,
        std::function<void(const std::string&)> onChunk = nullptr);

    va::network::HttpResponse httpGet(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        int timeoutMs);

    //  Network event hooks - override in subclass 

    // Fired per streaming chunk (SSE) or for the full response body
    virtual void onHttpText(const std::string& text);

    // Fired for binary response bodies
    virtual void onHttpBytes(const std::vector<uint8_t>& bytes);

    // Fired on any network error; default: setState(ERROR) + emit ErrorFrame + fire onError
    virtual void onHttpError(const va::network::NetworkError& error);

    std::shared_ptr<va::network::HttpInterface> _httpClient;

private:
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _running{false};
};

} // namespace transport
} // namespace va
