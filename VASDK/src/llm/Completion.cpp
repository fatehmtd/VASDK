#include "llm/Completion.h"

#include "network/HttpInterface.h"
#include "network/providers/CurlHttpProvider.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace va {
namespace llm {

namespace {
const char* roleName(MessageRole role) {
    switch (role) {
    case MessageRole::SYSTEM: return "system";
    case MessageRole::ASSISTANT: return "assistant";
    case MessageRole::TOOL: return "tool";
    case MessageRole::USER: break;
    }
    return "user";
}
} // namespace

OpenAICompletion::OpenAICompletion(LLMConfig config, std::shared_ptr<network::HttpInterface> http)
    : _config(std::move(config)), _http(std::move(http)) {
    if (!_http) _http = std::make_shared<network::providers::CurlHttpProvider>();
}

std::optional<std::string> OpenAICompletion::complete(const std::vector<ConversationMessage>& messages) const {
    return completeWithin(messages, 0);
}

std::optional<std::string> OpenAICompletion::completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const {
    const int timeoutMs = timeLimitMs > 0 && (_config.timeoutMs <= 0 || timeLimitMs < _config.timeoutMs) ? timeLimitMs : _config.timeoutMs;
    nlohmann::json payload{{"model", _config.model}, {"temperature", _config.temperature}, {"max_tokens", _config.maxTokens}};
    if (!_config.reasoningEffort.empty()) payload["reasoning_effort"] = _config.reasoningEffort;
    auto& list = payload["messages"] = nlohmann::json::array();
    for (const auto& message : messages) list.push_back({{"role", roleName(message.role)}, {"content", message.content}});

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!_config.apiKey.empty()) headers["Authorization"] = "Bearer " + _config.apiKey;
    for (const auto& [key, value] : _config.headers) headers[key] = value;

    const auto response = _http->post(_config.endpoint, headers, payload.dump(), timeoutMs);
    if (response.statusCode < 200 || response.statusCode >= 300) {
        spdlog::warn("[Completion] {} answered {}: {}", _config.endpoint, response.statusCode,
                     response.body.empty() ? response.error.message : response.body.substr(0, 300));
        return std::nullopt;
    }
    try {
        const auto body = nlohmann::json::parse(response.body);
        const auto& content = body.at("choices").at(0).at("message").at("content");
        if (content.is_string()) return content.get<std::string>();
    } catch (const std::exception& error) {
        spdlog::warn("[Completion] unreadable reply from {}: {}", _config.endpoint, error.what());
        return std::nullopt;
    }
    spdlog::warn("[Completion] {} replied without text", _config.endpoint);
    return std::nullopt;
}

std::string jsonObjectIn(const std::string& reply) {
    const auto start = reply.find('{');
    if (start == std::string::npos) return {};
    // The object ends where its braces balance, ignoring braces inside strings.
    int depth = 0;
    bool inString = false, escaped = false;
    for (size_t i = start; i < reply.size(); ++i) {
        const char c = reply[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return reply.substr(start, i - start + 1);
        }
    }
    return {};
}

} // namespace llm
} // namespace va
