#include <VASDK/llm/AnthropicTransport.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace va {
namespace llm {

AnthropicTransport::AnthropicTransport(const LLMConfig& config)
    : LLMTransport(LLMProvider::ANTHROPIC, config) {
}

std::string AnthropicTransport::buildRequestPayload(const std::string& userMessage) {
    json payload;
    payload["model"] = _config.model;
    payload["max_tokens"] = _config.maxTokens;
    payload["temperature"] = _config.temperature;

    // render() produces {"system":"...","messages":[...]} - unpack both fields directly
    std::string rendered = renderTemplate();
    const bool useTemplate = !rendered.empty();

    std::string systemPrompt;
    json messages = json::array();

    if (useTemplate) {
        json tmpl = json::parse(rendered);
        if (tmpl.contains("system")) systemPrompt = tmpl["system"].get<std::string>();
        if (tmpl.contains("messages")) messages = tmpl["messages"];
    }

    std::vector<ConversationMessage> history;
    {
        std::lock_guard<std::mutex> lock(_historyMutex);
        history = _conversationHistory;
    }

    for (const auto& msg : history) {
        if (msg.role == MessageRole::SYSTEM) {
            if (!useTemplate) systemPrompt = msg.content;
            continue;
        }
        messages.push_back({{"role", roleToString(msg.role)}, {"content", msg.content}});
    }

    if (!userMessage.empty()) {
        messages.push_back({{"role", "user"}, {"content", userMessage}});
    }

    if (!systemPrompt.empty()) payload["system"] = systemPrompt;
    payload["messages"] = messages;
    if (_config.streaming) payload["stream"] = true;

    return payload.dump();
}

std::string AnthropicTransport::parseResponse(const std::string& response) {
    try {
        json j = json::parse(response);

        if (j.contains("error")) {
            std::string errorMsg = j["error"].value("message", "Unknown error");
            if (onError) {
                onError(static_cast<int>(LLMErrorCode::AnthropicApiError), "Anthropic API error: " + errorMsg);
            }
            return "";
        }

        if (j.contains("content") && !j["content"].empty()) {
            auto& content = j["content"][0];
            if (content.contains("text")) {
                return content["text"].get<std::string>();
            }
        }

        return "";

    } catch (const std::exception& e) {
        if (onError) {
            onError(static_cast<int>(LLMErrorCode::AnthropicParseFailed), std::string("Failed to parse Anthropic response: ") + e.what());
        }
        return "";
    }
}

} // namespace llm
} // namespace va
