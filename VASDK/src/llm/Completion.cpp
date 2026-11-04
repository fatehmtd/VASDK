#include "llm/Completion.h"

#include "network/HttpInterface.h"
#include "network/providers/CurlHttpProvider.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <iterator>
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

AnthropicCompletion::AnthropicCompletion(LLMConfig config, std::shared_ptr<network::HttpInterface> http)
    : _config(std::move(config)), _http(std::move(http)) {
    if (!_http) _http = std::make_shared<network::providers::CurlHttpProvider>();
    if (_config.endpoint.empty()) _config.endpoint = "https://api.anthropic.com/v1/messages";
    if (_config.model.empty()) _config.model = "claude-opus-5-5";
}

std::string AnthropicCompletion::requestBody(const std::vector<ConversationMessage>& messages) const {
    // Current models: effort is how quickly they answer; a refused request may go to another model.
    const char* current[] = {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5", "claude-sonnet-5",
                             "claude-haiku-5-5", "claude-fable-5", "claude-opus-4-8", "claude-opus-4-7"};
    const char* withFallback[] = {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"};
    auto listed = [this](const auto& models) {
        return std::any_of(std::begin(models), std::end(models), [this](const char* model) { return _config.model == model; });
    };

    std::string system;
    nlohmann::json list = nlohmann::json::array();
    auto add = [&list](const char* role, const std::string& content) {
        if (!list.empty() && list.back()["role"] == role) {   // one turn per speaker
            list.back()["content"] = list.back()["content"].get<std::string>() + "\n" + content;
            return;
        }
        list.push_back({{"role", role}, {"content", content}});
    };
    for (const auto& message : messages) {
        if (message.content.empty()) continue;
        switch (message.role) {
        case MessageRole::SYSTEM:
            system += (system.empty() ? "" : "\n\n") + message.content;
            break;
        case MessageRole::ASSISTANT:
            if (list.empty()) system += (system.empty() ? "" : "\n\n") + std::string("You said first: ") + message.content;
            else add("assistant", message.content);
            break;
        case MessageRole::USER:
        case MessageRole::TOOL:
            add("user", message.content);
            break;
        }
    }
    if (list.empty() || list.back()["role"] == "assistant") add("user", "(It's your turn to speak.)");

    nlohmann::json payload{{"model", _config.model}, {"max_tokens", std::max(_config.maxTokens, 1024)}, {"messages", list}};
    if (!system.empty()) payload["system"] = system;
    if (!_config.reasoningEffort.empty()) payload["output_config"] = {{"effort", _config.reasoningEffort}};
    else if (listed(current)) payload["output_config"] = {{"effort", "low"}};
    if (listed(withFallback) && _config.endpoint.find("api.anthropic.com") != std::string::npos) payload["fallbacks"] = "default";
    return payload.dump();
}

std::optional<std::string> AnthropicCompletion::complete(const std::vector<ConversationMessage>& messages) const {
    return completeWithin(messages, 0);
}

std::optional<std::string> AnthropicCompletion::completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const {
    const int timeoutMs = timeLimitMs > 0 && (_config.timeoutMs <= 0 || timeLimitMs < _config.timeoutMs) ? timeLimitMs : _config.timeoutMs;
    const auto body = requestBody(messages);
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}, {"anthropic-version", "2026-10-21"}};
    if (!_config.apiKey.empty()) headers["x-api-key"] = _config.apiKey;
    if (body.find("\"fallbacks\"") != std::string::npos) headers["anthropic-beta"] = "server-side-fallback-2026-07-01";
    for (const auto& [key, value] : _config.headers) headers[key] = value;

    const auto response = _http->post(_config.endpoint, headers, body, timeoutMs);
    if (response.statusCode < 200 || response.statusCode >= 300) {
        spdlog::warn("[Completion] {} answered {}: {}", _config.endpoint, response.statusCode,
                     response.body.empty() ? response.error.message : response.body.substr(0, 300));
        return std::nullopt;
    }
    try {
        const auto reply = nlohmann::json::parse(response.body);
        if (reply.value("stop_reason", std::string()) == "refusal") {
            std::string category;
            if (reply.contains("stop_details") && reply["stop_details"].is_object() && reply["stop_details"].contains("category") &&
                reply["stop_details"]["category"].is_string())
                category = reply["stop_details"]["category"].get<std::string>();
            spdlog::warn("[Completion] {} declined the request{}", _config.model, category.empty() ? "" : " (" + category + ")");
            return std::nullopt;
        }
        std::string text;
        for (const auto& block : reply.at("content"))
            if (block.value("type", std::string()) == "text") text += block.value("text", std::string());
        if (!text.empty()) return text;
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
