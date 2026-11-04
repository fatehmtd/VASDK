#include "flow/Tools.h"

#include "flow/CompiledAgent.h"
#include "network/HttpInterface.h"
#ifdef VASDK_HAS_CURL
#include "network/providers/CurlHttpProvider.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <thread>

namespace va {
namespace flow {

namespace {

constexpr int kDefaultTimeoutMs = 8000;
constexpr int kDefaultMaxResponseKb = 256;

ToolResult failed(std::string error) {
    ToolResult result;
    result.error = std::move(error);
    return result;
}

ToolResult succeeded(nlohmann::json value) {
    // A tool reports its own failure as {"error": ...}, as LLM tools do.
    if (value.is_object() && value.contains("error"))
        return failed(value["error"].is_string() ? value["error"].get<std::string>() : value["error"].dump());
    ToolResult result;
    result.ok = true;
    result.value = std::move(value);
    return result;
}

// * matches any run of characters, everything else itself.
bool globMatch(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string queryOf(const nlohmann::json& args) {
    std::string query;
    for (const auto& [key, value] : args.items()) {
        if (!query.empty()) query += '&';
        query += urlEncode(key) + '=' + urlEncode(value.is_string() ? value.get<std::string>() : value.dump());
    }
    return query;
}

// $secrets.<name> in a header, from the engine config.
bool withSecrets(std::string& text, const google::protobuf::Map<std::string, std::string>& secrets, std::string& error) {
    static const std::string kPrefix = "$secrets.";
    for (size_t at = text.find(kPrefix); at != std::string::npos; at = text.find(kPrefix, at)) {
        size_t end = at + kPrefix.size();
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
        const std::string name = text.substr(at + kPrefix.size(), end - at - kPrefix.size());
        const auto found = secrets.find(name);
        if (found == secrets.end()) {
            error = "no secret \"" + name + "\" in the engine config's security.secrets";
            return false;
        }
        text.replace(at, end - at, found->second);
        at += found->second.size();
    }
    return true;
}

} // namespace

ToolRunner::ToolRunner(std::shared_ptr<const CompiledAgent> agent, vasdk::engine::Security security,
                       std::shared_ptr<network::HttpInterface> http)
    : _agent(std::move(agent)), _security(std::move(security)), _http(std::move(http)) {
#ifdef VASDK_HAS_CURL
    if (!_http) {
        auto curl = std::make_shared<network::providers::CurlHttpProvider>();
        curl->setFollowRedirects(false);   // a redirect could lead outside http_allow
        curl->setVerifyTls(true);
        curl->setMaxResponseBytes(static_cast<size_t>(
            _security.has_max_response_kb() ? _security.max_response_kb() : kDefaultMaxResponseKb) * 1024);
        _http = std::move(curl);
    }
#endif
}

ToolCaller ToolRunner::caller(std::shared_ptr<const ToolRunner> runner) {
    return [runner = std::move(runner)](const std::string& name, const nlohmann::json& args) { return runner->call(name, args); };
}

bool ToolRunner::allowed(const google::protobuf::RepeatedPtrField<std::string>& patterns, const std::string& url) {
    // "https://allowed.example@elsewhere.example/" names elsewhere: no user info, no spaces.
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const auto authorityEnd = url.find_first_of("/?#", scheme + 3);
    if (url.substr(scheme + 3, authorityEnd - (scheme + 3)).find('@') != std::string::npos) return false;
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= ' ' || c == 127; })) return false;
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) { return globMatch(pattern, url); });
}

ToolResult ToolRunner::call(const std::string& name, const nlohmann::json& args) const {
    const auto timeout = std::chrono::milliseconds(_security.has_timeout_ms() ? _security.timeout_ms() : kDefaultTimeoutMs);
    if (const auto* tool = _agent->registry().tool(name)) {
        // On its own thread, so a stuck tool can't hold the conversation past the timeout.
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        std::thread([tool = *tool, args, promise] {
            try {
                promise->set_value(tool.execute(args));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        }).detach();
        if (future.wait_for(timeout) != std::future_status::ready)
            return failed("still running after " + std::to_string(timeout.count()) + " ms");
        try {
            return succeeded(future.get());
        } catch (const std::exception& error) {
            return failed(error.what());
        } catch (...) {
            return failed("failed");
        }
    }
    for (const auto& tool : _agent->definition().tools()) {
        if (tool.name() != name) continue;
        if (!tool.has_http()) return failed("\"" + name + "\" is provided by the app, and this app has none");
        return callHttp(tool, args);
    }
    return failed("no tool \"" + name + "\"");
}

ToolResult ToolRunner::callHttp(const vasdk::flow::Tool& tool, const nlohmann::json& args) const {
    const auto& http = tool.http();
    const bool get = http.method() == "GET";
    std::string url = http.url();
    if (get && !args.empty()) url += (url.find('?') == std::string::npos ? "?" : "&") + queryOf(args);
    if (!allowed(_security.http_allow(), url)) return failed("\"" + http.url() + "\" is not in the engine config's security.http_allow");
    if (!_http) return failed("this build has no HTTP client");

    std::map<std::string, std::string> headers;
    for (const auto& [header, value] : http.headers()) {
        std::string filled = value, error;
        if (!withSecrets(filled, _security.secrets(), error)) return failed(error);
        headers[header] = filled;
    }
    const int timeoutMs = _security.has_timeout_ms() ? _security.timeout_ms() : kDefaultTimeoutMs;
    network::HttpResponse response;
    if (get) {
        response = _http->get(url, headers, timeoutMs);
    } else {
        headers.emplace("Content-Type", "application/json");
        response = _http->post(url, headers, args.dump(), timeoutMs);
    }
    if (response.error) return failed(response.error.message);
    if (response.statusCode < 200 || response.statusCode >= 300)
        return failed("HTTP " + std::to_string(response.statusCode));
    const size_t maxBytes = static_cast<size_t>(_security.has_max_response_kb() ? _security.max_response_kb() : kDefaultMaxResponseKb) * 1024;
    if (response.body.size() > maxBytes) return failed("the reply is longer than " + std::to_string(maxBytes / 1024) + " KB");
    auto value = nlohmann::json::parse(response.body, nullptr, false);
    return succeeded(value.is_discarded() ? nlohmann::json(response.body) : std::move(value));
}

std::string maskValue(const std::string& value) {
    const auto digits = std::count_if(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    long keep = digits >= 8 ? 4 : 0;   // a phone or card number keeps its last 4 digits, as receipts do
    std::string masked = value;
    for (auto c = masked.rbegin(); c != masked.rend(); ++c) {
        const auto character = static_cast<unsigned char>(*c);
        if (std::isdigit(character) && keep > 0) --keep;
        else if (std::isalnum(character)) *c = '*';
    }
    return masked;
}

std::string redact(std::string text, const std::vector<std::string>& values) {
    std::vector<std::string> longestFirst = values;
    std::sort(longestFirst.begin(), longestFirst.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    for (const auto& value : longestFirst) {
        if (value.size() < 2) continue;
        const std::string masked = maskValue(value);
        for (size_t at = text.find(value); at != std::string::npos; at = text.find(value, at + masked.size()))
            text.replace(at, value.size(), masked);
    }
    return text;
}

} // namespace flow
} // namespace va
