#pragma once

#include <VASDK/llm/LLMTransport.h>

namespace va {
namespace llm {

class VASDK_EXPORT AnthropicTransport : public LLMTransport {
public:
    explicit AnthropicTransport(const LLMConfig& config);

    std::string getName() const override { return "AnthropicTransport"; }
    std::string getDescription() const override {
        return "Transport for Anthropic Claude models (Claude 3, etc.)";
    }

protected:
    std::string buildRequestPayload(const std::string& userMessage) override;
    std::string parseResponse(const std::string& response) override;
};

} // namespace llm
} // namespace va
