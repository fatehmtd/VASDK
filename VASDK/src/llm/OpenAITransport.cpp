#include <VASDK/llm/OpenAITransport.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace {
void logIfTruncatedByLength(const nlohmann::json& choice) {
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string() &&
        choice["finish_reason"].get<std::string>() == "length") {
        spdlog::warn("[OpenAITransport] LLM response truncated by provider "
                     "(finish_reason=\"length\") - generation hit max_tokens before completing");
    }
}
} // namespace

namespace va {
namespace llm {

OpenAITransport::OpenAITransport(const LLMConfig& config)
    : LLMTransport(LLMProvider::OPENAI, config) {
}

std::string OpenAITransport::buildRequestPayload(const std::string& userMessage) {
    json payload;
    payload["model"] = _config.model;
    payload["temperature"] = _config.temperature;
    payload["max_tokens"] = _config.maxTokens;

    // render() owns the format logic; parse the resulting array as the base messages
    std::string rendered = renderTemplate();
    const bool useTemplate = !rendered.empty();

    json messages = useTemplate ? json::parse(rendered) : json::array();

    std::vector<ConversationMessage> history;
    {
        std::lock_guard<std::mutex> lock(_historyMutex);
        history = _conversationHistory;
    }

    for (const auto& msg : history) {
        if (useTemplate && msg.role == MessageRole::SYSTEM) continue;
        messages.push_back({{"role", roleToString(msg.role)}, {"content", msg.content}});
    }

    // Note: userMessage is already in _conversationHistory (added by processIncomingFrame
    // before sendLLMRequest is called), so we do NOT append it again here.

    payload["messages"] = messages;
    if (_config.streaming) payload["stream"] = true;

    return payload.dump();
}

std::string OpenAITransport::parseResponse(const std::string& response) {
    try {
        json j = json::parse(response);

        if (j.contains("error")) {
            std::string errorMsg = j["error"].value("message", "Unknown error");
            if (onError) {
                onError(static_cast<int>(LLMErrorCode::OpenAIApiError), "OpenAI API error: " + errorMsg);
            }
            return "";
        }

        if (j.contains("choices") && !j["choices"].empty()) {
            auto& choice = j["choices"][0];
            logIfTruncatedByLength(choice);
            if (choice.contains("message") && choice["message"].contains("content")) {
                return choice["message"]["content"].get<std::string>();
            }
        }

        return "";

    } catch (const std::exception& e) {
        if (onError) {
            onError(static_cast<int>(LLMErrorCode::OpenAIParseFailed), std::string("Failed to parse OpenAI response: ") + e.what());
        }
        return "";
    }
}

bool OpenAITransport::parseStreamingData(const std::string& data,
                                         std::string& deltaText,
                                         bool& isDone) {
    deltaText.clear();
    isDone = false;

    if (data == "[DONE]") {
        isDone = true;
        return true;
    }

    try {
        json j = json::parse(data);

        if (j.contains("error")) {
            std::string errorMsg = j["error"].value("message", "Unknown error");
            if (onError) {
                onError(static_cast<int>(LLMErrorCode::OpenAIApiError), "OpenAI API error: " + errorMsg);
            }
            return true;
        }

        if (!j.contains("choices") || j["choices"].empty()) {
            return true;
        }

        const auto& choice = j["choices"][0];

        if (choice.contains("delta") && choice["delta"].contains("content") &&
            choice["delta"]["content"].is_string()) {
            deltaText = choice["delta"]["content"].get<std::string>();
        }

        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            isDone = true;
            logIfTruncatedByLength(choice);
        }

        return true;

    } catch (const std::exception& e) {
        if (onError) {
            onError(static_cast<int>(LLMErrorCode::OpenAIParseFailed), std::string("Failed to parse OpenAI stream chunk: ") + e.what());
        }
        return false;
    }
}

} // namespace llm
} // namespace va
