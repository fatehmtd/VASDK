#pragma once

#include <VASDK/llm/PromptTemplate.h>

namespace va {
namespace llm {

// JSON-encoded formats for hosted-API providers. render() requires VASDK_HAS_JSON at build time.

// Shared base for the hosted-API formats: hosted models follow instructions
// reliably, so renderToolPrompt() emits the same {"text" | "tools"} protocol as
// the PromptTemplate default but without the few-shot examples and repeated
// warnings small local models need.
class VASDK_EXPORT ApiPromptTemplate : public PromptTemplate {
public:
    std::string renderToolPrompt(const std::vector<const Tool*>& enabledTools,
                                 const std::string& textFieldGuidance) const override;

protected:
    ApiPromptTemplate() = default;
};

class VASDK_EXPORT OpenAIPromptTemplate : public ApiPromptTemplate {
public:
    OpenAIPromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::OPENAI; }
    std::string render() const override;
};

class VASDK_EXPORT AnthropicPromptTemplate : public ApiPromptTemplate {
public:
    AnthropicPromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::ANTHROPIC; }
    std::string render() const override;
};

class VASDK_EXPORT GooglePromptTemplate : public ApiPromptTemplate {
public:
    GooglePromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::GOOGLE; }
    std::string render() const override;
};

} // namespace llm
} // namespace va
