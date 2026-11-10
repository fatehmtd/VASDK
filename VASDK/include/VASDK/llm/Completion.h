#pragma once

#include <VASDK/vasdk_export.h>
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

/**
 * @brief Claude, through the Anthropic Messages API (POST /v1/messages, anthropic-version
 *        2026-10-21). config.endpoint: the full URL, default https://api.anthropic.com/v1/messages;
 *        apiKey (x-api-key), model (default claude-opus-5-5), maxTokens (at least 1024: current
 *        models think before they answer), timeoutMs, headers.
 *
 * No sampling settings are sent (the API version rejects them). config.reasoningEffort is the
 * request's output_config.effort; unset, "low" on current models, as a voice turn wants a quick
 * answer. On models with a server-side fallback, a request a safety classifier declines is
 * retried on another model (fallbacks: "default"); a refusal still left is no reply.
 * System messages become the request's system prompt; a conversation can't open or end with
 * the assistant, so leading assistant words join the system prompt and a user turn closes it.
 */
class VASDK_EXPORT AnthropicCompletion : public Completion {
public:
    explicit AnthropicCompletion(LLMConfig config, std::shared_ptr<network::HttpInterface> http = nullptr);
    std::optional<std::string> complete(const std::vector<ConversationMessage>& messages) const override;
    std::optional<std::string> completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const override;
    /** The request body complete() sends, for tests and logs. */
    std::string requestBody(const std::vector<ConversationMessage>& messages) const;

private:
    LLMConfig _config;
    std::shared_ptr<network::HttpInterface> _http;
};

/** The first JSON object in an LLM's reply ("Sure! {...}", a ```json block): its text, or empty. */
VASDK_EXPORT std::string jsonObjectIn(const std::string& reply);

} // namespace llm
} // namespace va
