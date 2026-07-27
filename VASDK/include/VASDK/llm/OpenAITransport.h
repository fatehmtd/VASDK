#pragma once

#include <VASDK/llm/LLMTransport.h>

namespace va {
namespace llm {

class VASDK_EXPORT OpenAITransport : public LLMTransport {
public:
    explicit OpenAITransport(const LLMConfig& config);

    std::string getName() const override { return "OpenAITransport"; }
    std::string getDescription() const override {
        return "Transport for OpenAI-compatible chat completion APIs (OpenAI, OpenRouter, "
               "and self-hosted OpenAI-compatible servers)";
    }

protected:
    std::string buildRequestPayload(const std::string& userMessage) override;
    std::string parseResponse(const std::string& response) override;
    bool parseStreamingData(const std::string& data,
                            std::string& deltaText,
                            bool& isDone) override;
};

} // namespace llm
} // namespace va
