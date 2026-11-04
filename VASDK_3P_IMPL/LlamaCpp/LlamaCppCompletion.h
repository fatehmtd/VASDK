#pragma once

#include <VASDK/llm/Completion.h>
#include "vasdk_llamacpp_export.h"

#include <memory>
#include <mutex>

namespace va {
namespace transport {

/**
 * @brief An agent's LLM run in-process by llama.cpp: a GGUF model answers each question in
 *        full, with no network (llm::Completion, for nlu::LlmNluProvider, `llm` steps and
 *        generative pages).
 *
 * Configuration (llm::LLMConfig):
 *  - model: the GGUF file's path (required)
 *  - maxTokens: the longest answer, in tokens (default 400)
 *  - nCtx: the context window (default 4096); nThreads, nBatch, nGpuLayers as for LlamaCppTransport
 *  - temperature: 0 (the default) picks the likeliest token each time, so answers repeat;
 *    above 0 samples at that temperature
 *
 * The model loads in the constructor (loaded() says whether it did). Questions are answered one
 * at a time; each starts from an empty context, so no turn leaks into the next. A time limit
 * (completeWithin) is checked between tokens: past it, no answer. Thread-safe.
 */
class VASDK_LLAMACPP_EXPORT LlamaCppCompletion : public llm::Completion {
public:
    explicit LlamaCppCompletion(llm::LLMConfig config);
    ~LlamaCppCompletion() override;

    bool loaded() const;
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override;
    std::optional<std::string> completeWithin(const std::vector<llm::ConversationMessage>& messages, int timeLimitMs) const override;

private:
    struct Impl;
    llm::LLMConfig _config;
    std::unique_ptr<Impl> _impl;
    mutable std::mutex _mutex;   // one generation at a time
};

} // namespace transport
} // namespace va
