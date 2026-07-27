#include <VASDK/HttpTransport.h>
#include <VASDK/network/providers/CurlHttpProvider.h>
#include <spdlog/spdlog.h>

// Undefine Windows ERROR macro to avoid conflict with TransportState::ERROR
#ifdef ERROR
#undef ERROR
#endif

namespace va {
namespace transport {

HttpTransport::HttpTransport(std::shared_ptr<va::network::HttpInterface> client)
    : BaseTransport(), _httpClient(std::move(client)) {}

HttpTransport::~HttpTransport() {
    stop();
    release();
}

bool HttpTransport::initialize() {
    if (_initialized) return true;

    if (!_httpClient)
        _httpClient = std::make_shared<va::network::providers::CurlHttpProvider>();

    _httpClient->onTextReceived = [this](const std::string& text) {
        onHttpText(text);
    };
    _httpClient->onBytesReceived = [this](const std::vector<uint8_t>& bytes) {
        onHttpBytes(bytes);
    };
    _httpClient->onError = [this](const va::network::NetworkError& err) {
        onHttpError(err);
    };

    setState(TransportState::DISCONNECTED);
    _initialized = true;
    spdlog::info("[HttpTransport] Initialized");
    return true;
}

bool HttpTransport::start() {
    if (!_initialized && !initialize()) return false;
    if (_running) return true;

    _running = true;
    setState(TransportState::CONNECTED);
    fireOnConnected();
    spdlog::info("[HttpTransport] Started");
    return true;
}

void HttpTransport::stop() {
    if (!_running) return;
    _running = false;
    setState(TransportState::DISCONNECTED);
    fireOnDisconnected();
    spdlog::info("[HttpTransport] Stopped");
}

void HttpTransport::release() {
    stop();
    _initialized = false;
}

bool HttpTransport::isConnected() const {
    return _running && getState() == TransportState::CONNECTED;
}

va::network::HttpResponse HttpTransport::httpPost(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& body,
        int timeoutMs,
        std::function<void(const std::string&)> onChunk) {
    return _httpClient->post(url, headers, body, timeoutMs, std::move(onChunk));
}

va::network::HttpResponse HttpTransport::httpGet(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        int timeoutMs) {
    return _httpClient->get(url, headers, timeoutMs);
}

void HttpTransport::onHttpText(const std::string& /*text*/) {
    // Default: no-op. Subclasses override to handle streaming chunks.
}

void HttpTransport::onHttpBytes(const std::vector<uint8_t>& /*bytes*/) {
    // Default: no-op.
}

void HttpTransport::onHttpError(const va::network::NetworkError& error) {
    spdlog::error("[HttpTransport] Network error {}: {}", static_cast<int>(error.code), error.message);
    setState(TransportState::ERROR);

    auto errFrame = std::make_shared<ErrorFrame>();
    errFrame->errorCode    = error.nativeCode ? error.nativeCode : error.httpStatusCode;
    errFrame->errorMessage = error.message;
    errFrame->severity     = ErrorSeverity::ERROR;
    enqueueReceivedFrame(errFrame);

    fireOnError(errFrame->errorCode, error.message);
}

} // namespace transport
} // namespace va
