#pragma once

#include <string>
#include <map>
#include <cstdint>
#include "vasdk_export.h"

namespace va {
namespace llm {

struct LLMConfig {
    std::string apiKey;
    std::string endpoint;
    std::string model;
    std::string systemPrompt;
    float temperature = 0.7f;
    int maxTokens = 1000;
    int timeoutMs = 30000;
    bool streaming = false;
    /// OpenAI-style "reasoning_effort" ("none", "low", "medium", "high"), sent only when set.
    /// "none" stops a thinking model (Qwen3, gpt-oss, ...) from spending the token budget and
    /// the caller's time on a hidden reasoning trace before it answers.
    std::string reasoningEffort;
    std::map<std::string, std::string> headers;
    int nCtx = 4096;
    int nGpuLayers = 99;
    int nThreads = 0;
    int nBatch = 512;
    std::string nativeLibDir;

    // Extended sampling - used by LlamaCppTransport and LFM models.
    // Recommended for LFM2.5-1.2B-Instruct:  topK=50, repetitionPenalty=1.05
    // Recommended for LFM2.5-1.2B-Thinking:  topK=50, topP=0.1, repetitionPenalty=1.05
    float topP              = 1.0f;  ///< nucleus sampling threshold (1.0 = off)
    int   topK              = 0;     ///< top-K token limit (0 = off)
    float minP              = 0.0f;  ///< min-P filter: drop tokens below minP * p_max (0 = off)
    float repetitionPenalty = 1.0f;  ///< repetition penalty applied over last 64 tokens (1.0 = off)
};

enum class LLMProvider {
    OPENAI,
    ANTHROPIC,
    OPENROUTER,
    CUSTOM,
    LLAMA_CPP,
    LFM       ///< Liquid Foundation Model - local GGUF inference via llama.cpp
};

enum class MessageRole {
    SYSTEM,
    USER,
    ASSISTANT,
    TOOL      ///< Tool result message - used with LFM and other models that have a native tool role
};

struct ConversationMessage {
    MessageRole role;
    std::string content;
    uint64_t timestamp;
};

// Error codes passed as the first argument to Transport::onError()/fireOnError() by LLM
// transports, so consumers can switch on a stable code instead of matching message text.
enum class LLMErrorCode : int {
    MissingApiKey        = 1, ///< LLMTransport::initialize() - apiKey not configured
    InvalidEndpoint       = 2, ///< endpoint missing/malformed http(s) scheme, or contains whitespace
    EmptyResponse         = 3, ///< non-streaming LLM API call returned an empty body
    RequestFailed         = 4, ///< sendLLMRequest() threw (network/transport-level failure)
    ResponseParseFailed   = 5, ///< default LLMTransport::parseResponse() failed to parse the body
    OpenAIApiError        = 6, ///< OpenAI/OpenAI-compatible API returned an "error" field
    OpenAIParseFailed     = 7, ///< failed to parse an OpenAI response or stream chunk
    AnthropicApiError     = 8, ///< Anthropic API returned an "error" field
    AnthropicParseFailed  = 9, ///< failed to parse an Anthropic response
};

} // namespace llm
} // namespace va
