#include <VASDK/network/providers/CurlHttpProvider.h>
#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_CURL
#include <curl/curl.h>
#include <mutex>
#include <string_view>
#endif

namespace va {
namespace network {
namespace providers {

//  libcurl helpers 
#ifdef VASDK_HAS_CURL

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

struct StreamingContext {
    std::string* response;
    std::function<void(const std::string&)> onChunk;
};

static size_t streamingWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t bytes = size * nmemb;
    auto* ctx = static_cast<StreamingContext*>(userp);
    if (!ctx || !ctx->response) return 0;

    const char* chunk = static_cast<const char*>(contents);
    ctx->response->append(chunk, bytes);
    if (ctx->onChunk) ctx->onChunk(std::string(chunk, bytes));
    return bytes;
}

// curl_global_init must be called exactly once before any thread uses libcurl.
static std::once_flag sCurlInitFlag;
static void ensureCurlInit() {
    std::call_once(sCurlInitFlag, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

static bool supportsProtocol(const char* protocol) {
    if (!protocol) return false;
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;

    for (const char* const* p = info->protocols; *p; ++p) {
        if (std::string_view(*p) == protocol) return true;
    }
    return false;
}

static bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

static NetworkError curlCodeToError(CURLcode code, long httpStatus) {
    if (code != CURLE_OK) {
        NetworkError err;
        err.nativeCode = static_cast<int>(code);
        err.message    = curl_easy_strerror(code);
        switch (code) {
            case CURLE_OPERATION_TIMEDOUT:
                err.code = NetworkErrorCode::Timeout; break;
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY:
                err.code = NetworkErrorCode::ConnectionFailed; break;
            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_SSL_CERTPROBLEM:
            case CURLE_SSL_CIPHER:
            case CURLE_PEER_FAILED_VERIFICATION:
                err.code = NetworkErrorCode::TlsError; break;
            case CURLE_SEND_ERROR:
                err.code = NetworkErrorCode::SendFailed; break;
            case CURLE_RECV_ERROR:
                err.code = NetworkErrorCode::ReceiveFailed; break;
            default:
                err.code = NetworkErrorCode::LibraryError; break;
        }
        return err;
    }
    if (httpStatus >= 400) {
        NetworkError err;
        err.code           = NetworkErrorCode::HttpError;
        err.httpStatusCode = static_cast<int>(httpStatus);
        err.message        = "HTTP " + std::to_string(httpStatus);
        return err;
    }
    return {};
}

static HttpResponse performRequest(const std::string& url,
                                   const std::map<std::string, std::string>& hdrs,
                                   const std::string* body,
                                   int timeoutMs,
                                   const std::function<void(const std::string&)>& onChunk,
                                   const std::function<void(const NetworkError&)>& onError) {
    ensureCurlInit();

    HttpResponse result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = {NetworkErrorCode::LibraryError, "curl_easy_init failed"};
        if (onError) onError(result.error);
        return result;
    }

    // Required in multi-threaded apps - prevents signal handler installation (crashes on Android)
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    // WARNING: disables certificate verification - a workaround for cert errors common
    // in embedded Android environments. Production builds should bundle/trust proper CA certs.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VASDK/1.0");

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    StreamingContext streamCtx{&result.body, onChunk};
    if (onChunk) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamingWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamCtx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    }

    curl_slist* headerList = nullptr;
    for (const auto& [key, value] : hdrs) {
        std::string h = key + ": " + value;
        headerList = curl_slist_append(headerList, h.c_str());
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    CURLcode res = curl_easy_perform(curl);

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    result.statusCode = static_cast<int>(httpStatus);

    result.error = curlCodeToError(res, httpStatus);
    // Providers explain HTTP errors in the body (e.g. "Insufficient credits"); capped to keep logs sane.
    if (result.error.code == NetworkErrorCode::HttpError && !result.body.empty())
        result.error.message += ": " + result.body.substr(0, 1000);
    if (res == CURLE_UNSUPPORTED_PROTOCOL) {
        const std::string_view urlView(url);
        if (startsWith(urlView, "https://") && !supportsProtocol("https")) {
            result.error.code = NetworkErrorCode::ProtocolError;
            result.error.message =
                "Unsupported protocol: https is not available in this libcurl build "
                "(missing TLS backend). Enable HTTPS support for curl in the Android build "
                "or use an http:// endpoint.";
        } else {
            result.error.code = NetworkErrorCode::ProtocolError;
            result.error.message =
                "Unsupported URL protocol for endpoint: " + url;
        }
    }
    if (result.error && onError) onError(result.error);

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return result;
}

#endif // VASDK_HAS_CURL

//  CurlHttpProvider 

HttpResponse CurlHttpProvider::post(const std::string& url,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body,
                                    int timeoutMs,
                                    std::function<void(const std::string&)> onChunk) {
#ifdef VASDK_HAS_CURL
    auto resp = performRequest(url, headers, &body, timeoutMs, onChunk, onError);
    if (resp.error.ok() && onTextReceived && !resp.body.empty())
        onTextReceived(resp.body);
    return resp;
#else
    spdlog::error("[CurlHttpProvider] HTTP disabled (VASDK_HAS_CURL not set)");
    HttpResponse r;
    r.error = {NetworkErrorCode::LibraryError, "HTTP transport disabled in this build"};
    if (onError) onError(r.error);
    return r;
#endif
}

HttpResponse CurlHttpProvider::get(const std::string& url,
                                   const std::map<std::string, std::string>& headers,
                                   int timeoutMs) {
#ifdef VASDK_HAS_CURL
    auto resp = performRequest(url, headers, nullptr, timeoutMs, nullptr, onError);
    if (resp.error.ok() && onTextReceived && !resp.body.empty())
        onTextReceived(resp.body);
    return resp;
#else
    spdlog::error("[CurlHttpProvider] HTTP disabled (VASDK_HAS_CURL not set)");
    HttpResponse r;
    r.error = {NetworkErrorCode::LibraryError, "HTTP transport disabled in this build"};
    if (onError) onError(r.error);
    return r;
#endif
}

bool CurlHttpProvider::sendText(const std::string& url,
                                const std::map<std::string, std::string>& headers,
                                const std::string& text,
                                int timeoutMs) {
    auto hdrs = headers;
    hdrs["Content-Type"] = "text/plain";
    return post(url, hdrs, text, timeoutMs).error.ok();
}

bool CurlHttpProvider::sendBytes(const std::string& url,
                                 const std::map<std::string, std::string>& headers,
                                 const std::vector<uint8_t>& bytes,
                                 int timeoutMs) {
    auto hdrs = headers;
    hdrs["Content-Type"] = "application/octet-stream";
    std::string body(bytes.begin(), bytes.end());
    return post(url, hdrs, body, timeoutMs).error.ok();
}

} // namespace providers
} // namespace network
} // namespace va
