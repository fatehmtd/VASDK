#pragma once

#include "vasdk_export.h"
#include "LLMTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace network { class HttpInterface; }
namespace llm {

/**
 * @brief One question to an LLM, answered in full: the messages in, its text out. No history
 *        is kept between calls; the caller sends what the LLM must know. Thread-safe.
 *
 * Agents use it to understand what their rules can't (nlu::LlmNluProvider) and for `llm` steps
 * and generative pages (FlowRuntime::setLlm).
 */
class VASDK_EXPORT Completion {
public:
    virtual ~Completion() = default;
    /** The LLM's reply; nullopt, logged, when it can't be reached or fails. Blocks until it answers. */
    virtual std::optional<std::string> complete(const std::vector<ConversationMessage>& messages) const = 0;
    /**
     * The same, given up after timeLimitMs (when shorter than the client's own timeout): nullopt
     * then, as for a failure. For questions a turn can't wait long for (the intent check).
     * 0 or less: no limit beyond the client's. Default: complete(messages), the limit not kept.
     */
    virtual std::optional<std::string> completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const {
        (void)timeLimitMs;
        return complete(messages);
    }
};

/**
 * @brief An OpenAI-compatible chat completions endpoint (OpenAI, OpenRouter, LM Studio, vLLM,
 *        llama.cpp server, Ollama): config.endpoint is the full URL, as for OpenAITransport.
 *        Uses endpoint, apiKey, model, temperature, maxTokens, timeoutMs, reasoningEffort, headers.
 */
class VASDK_EXPORT OpenAICompletion : public Completion {
public:
    explicit OpenAICompletion(LLMConfig config, std::shared_ptr<network::HttpInterface> http = nullptr);
    std::optional<std::string> complete(const std::vector<ConversationMessage>& messages) const override;
    std::optional<std::string> completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const override;

private:
    LLMConfig _config;
    std::shared_ptr<network::HttpInterface> _http;
};

/** The first JSON object in an LLM's reply ("Sure! {...}", a ```json block): its text, or empty. */
VASDK_EXPORT std::string jsonObjectIn(const std::string& reply);

} // namespace llm
} // namespace va
