#pragma once

#include <VASDK/llm/LLMTypes.h>
#include "vasdk_export.h"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace va {
namespace llm {

class Tool;

enum class ChatFormat {
    // API formats - render() returns a JSON string
    OPENAI,      ///< [{"role":"system","content":"..."},...]
    ANTHROPIC,   ///< {"system":"...","messages":[...]}  (SYSTEM extracted to top-level field)
    GOOGLE,      ///< {"system_instruction":{...},"contents":[{"role":"user","parts":[{"text":"..."}]},...]}

    // Text formats - render() returns raw text with special tokens
    CHATML,      ///< <|im_start|>role\ncontent<|im_end|>\n   (Qwen, Yi, InternLM)
    LLAMA3,      ///< <|begin_of_text|>...<|eot_id|>          (Llama 3)
    LLAMA2,      ///< [INST] <<SYS>>\nsystem\n<</SYS>>\n\nuser [/INST] assistant
    MISTRAL,     ///< [INST] user [/INST] assistant            (no SYS block)
    GEMMA,       ///< <start_of_turn>role\ncontent<end_of_turn>\n
    PHI3,        ///< <|system|>\ncontent<|end|>\n<|user|>\ncontent<|end|>\n<|assistant|>\n
    LFM2,        ///< Liquid Foundation Model v2 - ChatML + <|startoftext|> prefix; tool results wrapped in <|tool_response_start|>…<|tool_response_end|>
    LFM2_5,      ///< Liquid Foundation Model v2.5 - ChatML + <|startoftext|> prefix; tool calls in <|tool_call_start|>…<|tool_call_end|>
};

enum class MissingVarPolicy {
    THROW,             ///< throw PromptTemplateError (default)
    LEAVE_PLACEHOLDER, ///< keep "{varName}" literally in output
    EMPTY_STRING,      ///< substitute "" silently
};

class VASDK_EXPORT PromptTemplateError : public std::runtime_error {
public:
    explicit PromptTemplateError(const std::string& msg) : std::runtime_error(msg) {}
};

// Message/variable builder shared by every provider-specific template. Each ChatFormat
// has its own subclass (ApiPromptTemplates.h / LocalPromptTemplates.h) implementing
// render(); use PromptTemplate::create(format) to obtain one.
class VASDK_EXPORT PromptTemplate {
public:
    virtual ~PromptTemplate() = default;

    // Builder - add turns
    PromptTemplate& addSystem(const std::string& tmplContent);
    PromptTemplate& addUser(const std::string& tmplContent);
    PromptTemplate& addAssistant(const std::string& tmplContent);
    PromptTemplate& addMessage(MessageRole role, const std::string& tmplContent);

    // Variable substitution
    PromptTemplate& set(const std::string& varName, const std::string& value);
    PromptTemplate& setAll(const std::unordered_map<std::string, std::string>& vars);
    PromptTemplate& unset(const std::string& varName);

    // Validation helpers
    std::vector<std::string> missingVariables() const;
    std::vector<std::string> unusedVariables() const;
    void setMissingVarPolicy(MissingVarPolicy policy);
    MissingVarPolicy getMissingVarPolicy() const;
    void clear();

    // Integration with existing transports - variables substituted, no format encoding applied
    std::vector<ConversationMessage> getMessages() const;

    // The ChatFormat this concrete template renders to.
    virtual ChatFormat format() const = 0;

    // Render to this template's target LLM format; throws PromptTemplateError on missing
    // variables (when policy == THROW). JSON formats require VASDK_HAS_JSON at build time.
    virtual std::string render() const = 0;

    // Wrapper tags for "thinking" output, e.g. "<think>"/"</think>" for DeepSeek-R1/QwQ.
    // Empty by default; override per-format (see ChatMLPromptTemplate, LFM2_5PromptTemplate).
    // Callers can still override per-model via LLMProcessor::setReasoningTags().
    virtual std::pair<std::string, std::string> reasoningTags() const { return {"", ""}; }

    // Wrapper tags some local formats expect around a tool-call payload, e.g.
    // LFM2/LFM2.5's "<|tool_call_start|>[...]<|tool_call_end|>". Empty by default.
    virtual std::pair<std::string, std::string> toolCallTags() const { return {"", ""}; }

    // System-prompt section listing enabled tools and how to call them. Override only
    // together with the matching parser: base impl emits the {"text"|"tools"} JSON
    // convention read by LLMProcessor::parseStructuredResponse(); ApiPromptTemplate
    // shortens it for hosted models; LFM subclasses emit their toolCallTags() form
    // instead, watched for by LlamaCppTransport. textFieldGuidance describes the "text"
    // field (see ToolDispatchProcessor::setTextFieldGuidance); ignored by formats
    // without one. Returns "" when enabledTools is empty. For the base JSON convention
    // this is the full response contract - don't append another format instruction after it.
    virtual std::string renderToolPrompt(const std::vector<const Tool*>& enabledTools,
                                         const std::string& textFieldGuidance) const;

    // Returns the idiomatic ChatFormat for API-backed providers.
    // Returns nullopt for LLAMA_CPP / CUSTOM (format is model-specific).
    static std::optional<ChatFormat> defaultFormatForProvider(LLMProvider provider);

    // Factory - constructs the concrete PromptTemplate subclass for the given format.
    static std::unique_ptr<PromptTemplate> create(ChatFormat format);

protected:
    PromptTemplate() = default;
    PromptTemplate(const PromptTemplate&) = default;
    PromptTemplate& operator=(const PromptTemplate&) = default;

    struct TemplateMessage {
        MessageRole _role;
        std::string _templateContent;
    };

    std::string substituteVars(const std::string& tmpl) const;

    // Resolves every message's variables; warns if the template is empty.
    std::vector<ConversationMessage> resolveAll() const;

    std::vector<TemplateMessage> _messages;
    std::unordered_map<std::string, std::string> _variables;
    MissingVarPolicy _missingVarPolicy{MissingVarPolicy::THROW};
};

} // namespace llm
} // namespace va
