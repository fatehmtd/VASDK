#pragma once

#include <VASDK/llm/PromptTemplate.h>

namespace va {
namespace llm {

// Plain-text formats with special tokens, for locally-hosted / self-templated models.

class VASDK_EXPORT ChatMLPromptTemplate : public PromptTemplate {
public:
    ChatMLPromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::CHATML; }
    std::string render() const override;

    // CHATML is the wire format for Qwen3/QwQ/DeepSeek-R1-family models, which
    // popularized the <think>...</think> reasoning-wrapper convention. Harmless
    // no-op for non-reasoning CHATML models - the tags simply won't appear.
    std::pair<std::string, std::string> reasoningTags() const override {
        return {"<think>", "</think>"};
    }
};

class VASDK_EXPORT Llama3PromptTemplate : public PromptTemplate {
public:
    Llama3PromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::LLAMA3; }
    std::string render() const override;
};

class VASDK_EXPORT Llama2PromptTemplate : public PromptTemplate {
public:
    Llama2PromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::LLAMA2; }
    std::string render() const override;
};

class VASDK_EXPORT MistralPromptTemplate : public PromptTemplate {
public:
    MistralPromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::MISTRAL; }
    std::string render() const override;
};

class VASDK_EXPORT GemmaPromptTemplate : public PromptTemplate {
public:
    GemmaPromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::GEMMA; }
    std::string render() const override;
};

class VASDK_EXPORT Phi3PromptTemplate : public PromptTemplate {
public:
    Phi3PromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::PHI3; }
    std::string render() const override;
};

class VASDK_EXPORT LFM2PromptTemplate : public PromptTemplate {
public:
    LFM2PromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::LFM2; }
    std::string render() const override;

    // LFM2 wraps native tool calls in <|tool_call_start|>[...]<|tool_call_end|>.
    std::pair<std::string, std::string> toolCallTags() const override {
        return {"<|tool_call_start|>", "<|tool_call_end|>"};
    }

    // Tool list wrapped in the native <|tool_list_start|>…<|tool_list_end|> tokens,
    // plus the toolCallTags() call-format instruction. No "text" field: LFM tool
    // calls are native, not the JSON {"text" | "tools"} protocol.
    std::string renderToolPrompt(const std::vector<const Tool*>& enabledTools,
                                 const std::string& textFieldGuidance) const override;
};

class VASDK_EXPORT LFM2_5PromptTemplate : public PromptTemplate {
public:
    LFM2_5PromptTemplate() = default;

    ChatFormat format() const override { return ChatFormat::LFM2_5; }
    std::string render() const override;

    // LFM2.5-Thinking emits <think>...</think> around its reasoning trace before
    // the final answer. No-op for the non-thinking LFM2.5-Instruct checkpoint.
    std::pair<std::string, std::string> reasoningTags() const override {
        return {"<think>", "</think>"};
    }

    // Same native tool-call wrapper tokens as LFM2.
    std::pair<std::string, std::string> toolCallTags() const override {
        return {"<|tool_call_start|>", "<|tool_call_end|>"};
    }

    // "List of tools: [...]" - replicates the string the model's Jinja chat template
    // generates natively when tools are passed via the llama.cpp tools interface -
    // plus the toolCallTags() call-format instruction.
    std::string renderToolPrompt(const std::vector<const Tool*>& enabledTools,
                                 const std::string& textFieldGuidance) const override;
};

} // namespace llm
} // namespace va
