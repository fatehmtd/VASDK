#pragma once

#include <VASDK/llm/LLMTransport.h>
#include "vasdk_llamacpp_export.h"
#include "ResponseChunker.h"
#include <memory>

namespace va {
namespace transport {

/**
 * @brief LLM transport for local, offline inference using llama.cpp
 *
 * LlamaCppTransport runs LLM inference entirely on-device by loading a GGUF
 * model file via llama.cpp.  No API key or network connection is required.
 *
 * Configuration:
 *  - config.model       - absolute path to a GGUF model file (required)
 *  - config.systemPrompt - system / persona prompt
 *  - config.temperature - sampling temperature (default 0.7)
 *  - config.maxTokens   - maximum tokens to generate per turn (default 1000)
 *  - config.nCtx        - context window size in tokens (default 4096)
 *
 * config.apiKey and config.endpoint are not used and may be left empty.
 *
 * Example:
 * @code
 * va::llm::LLMConfig cfg;
 * cfg.model       = "/models/qwen2.5-1.5b-instruct-q4_k_m.gguf";
 * cfg.systemPrompt = "You are a helpful voice assistant.";
 * cfg.temperature  = 0.2f;
 * cfg.maxTokens    = 512;
 * cfg.nCtx         = 4096;
 *
 * auto transport = std::make_shared<va::transport::LlamaCppTransport>(cfg);
 * transport->initialize();
 * transport->start();
 * @endcode
 */
class VASDK_LLAMACPP_EXPORT LlamaCppTransport : public llm::LLMTransport {
public:
    explicit LlamaCppTransport(const llm::LLMConfig& config);
    ~LlamaCppTransport();

    // LLMTransport / Transport overrides
    bool        initialize() override;
    void        release()    override;
    std::string getName()        const override { return "LlamaCppTransport"; }
    std::string getDescription() const override {
        return "Local llama.cpp LLM transport for offline inference";
    }

    /**
     * @brief Cancel the currently running generation (e.g. on barge-in).
     *
     * Thread-safe.  The running sendLLMRequest() will finish the current token
     * and return an empty string on the next iteration.
     */
    void cancel() override;

    /**
     * @brief Clear conversation history and reset the KV cache state.
     *
     * Overrides the base implementation to also wipe the incremental KV cache
     * tracking variables (kvPos / cachedFormattedLen) so that the next request
     * re-encodes the full prompt from scratch rather than computing a stale delta.
     */
    void clearHistory() override;

    /**
     * @brief Add a message and reset KV if history trimming removes an entry.
     *
     * When the conversation history reaches _maxHistoryLength the base class
     * evicts the oldest non-system message.  The KV cache was built for the
     * (now discarded) longer history, so cachedFormattedLen is stale.  We
     * detect the trim here and call resetKV() in sync so the next call to
     * sendLLMRequest() re-encodes the full (trimmed) history instead of
     * triggering the "KV cache state inconsistent" fallback path.
     */
    void addMessageToHistory(llm::MessageRole role, const std::string& content) override;

    /**
     * @brief Replace the response chunking strategy used during generation.
     *
     * The chunker controls how streaming output is partitioned into frames that
     * are forwarded to downstream processors (e.g. TTSFrameProcessor) before
     * the full response has finished generating.
     *
     * Built-in options (see ResponseChunker.h):
     *   - PunctuationChunker  - splits on sentence-ending punctuation (default)
     *   - WordCountChunker    - splits every N words
     *   - NoChunkingChunker   - emits the entire response as one frame at the end
     *
     * @param chunker  Non-null chunker; null is ignored.
     */
    void setChunker(std::shared_ptr<ResponseChunkerInterface> chunker) {
        if (chunker) chunker_ = std::move(chunker);
    }

    const ResponseChunkerInterface& chunker() const { return *chunker_; }

protected:
    std::string sendLLMRequest(const std::string& userMessage) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<ResponseChunkerInterface> chunker_{ std::make_shared<NoChunkingChunker>() };
};

} // namespace transport
} // namespace va
