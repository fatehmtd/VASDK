#include "LlamaCppTransport.h"

#include <VASDK/llm/PromptTemplate.h>
#include <llama.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// Undefine Windows ERROR macro that conflicts with TransportState::ERROR
#ifdef ERROR
#undef ERROR
#endif

namespace va {
namespace transport {

// ---------------------------------------------------------------------------
// Impl - holds all llama.cpp state; defined here to keep llama.h off the
// public header (Pimpl idiom).
// ---------------------------------------------------------------------------
struct LlamaCppTransport::Impl {
    llama_model*   model   = nullptr;
    llama_context* ctx     = nullptr;
    llama_sampler* sampler = nullptr;
    bool           backendInited = false;

    std::atomic<bool>  cancelRequested{false};
    std::vector<char>  fmtBuf{std::vector<char>(8192, 0)}; // persistent chat-template buffer

    // -----------------------------------------------------------------------
    // KV-cache position tracking.
    // cachedFormattedLen is the byte length of the formatted chat string that
    // has already been decoded into the KV cache.  It equals:
    //   format(full_history_after_last_completed_turn, add_ass=false).size()
    // Each new turn appends only the suffix beyond this offset.
    // -----------------------------------------------------------------------
    llama_pos kvPos             = 0;  // next write position in the KV cache
    size_t    cachedFormattedLen = 0; // byte offset into formatted string already decoded

    ~Impl() { destroy(); }

    void destroy() {
        if (sampler) { llama_sampler_free(sampler); sampler = nullptr; }
        if (ctx)     { llama_free(ctx);             ctx     = nullptr; }
        if (model)   { llama_model_free(model);     model   = nullptr; }
        if (backendInited) { llama_backend_free(); backendInited = false; }
    }

    // Clear KV cache and reset position state (used on overflow or inconsistency).
    void resetKV() {
        if (ctx) llama_memory_clear(llama_get_memory(ctx), false);
        kvPos              = 0;
        cachedFormattedLen = 0;
    }

    // Apply chat template and return the formatted string, resizing fmtBuf as needed.
    std::string applyTemplate(const char* tmpl,
                              const std::vector<llama_chat_message>& msgs,
                              bool add_ass) {
        if (msgs.empty()) return {};
        int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), add_ass,
                                          fmtBuf.data(), static_cast<int32_t>(fmtBuf.size()));
        if (n > static_cast<int>(fmtBuf.size())) {
            fmtBuf.resize(static_cast<size_t>(n) + 1);
            n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), add_ass,
                                          fmtBuf.data(), static_cast<int32_t>(fmtBuf.size()));
        }
        return n > 0 ? std::string(fmtBuf.data(), static_cast<size_t>(n)) : std::string{};
    }

    // Tokenise a UTF-8 string into a std::vector<llama_token>.
    static std::vector<llama_token> tokenize(
            const llama_vocab* vocab,
            const std::string& text,
            bool add_special,
            bool parse_special) {
        int n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                               nullptr, 0, add_special, parse_special);
        if (n == 0) return {};
        const int need = (n < 0) ? -n : n;
        std::vector<llama_token> out(static_cast<size_t>(need));
        int actual = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                                    out.data(), need, add_special, parse_special);
        if (actual < 0) return {};
        out.resize(static_cast<size_t>(actual));
        return out;
    }

    // Decode a token vector in n_batch-sized chunks, advancing kvPos.
    // Returns false on failure; on context overflow resets KV and returns false
    // so the caller can retry with a full re-encode.
    bool decodeTokens(
            const std::vector<llama_token>& tokens,
            int n_ctx,
            int max_gen_tokens,
            bool compute_last_logit = false) {
        const int batch_cap = static_cast<int>(llama_n_batch(ctx));
        const int n         = static_cast<int>(tokens.size());

        for (int i = 0; i < n; i += batch_cap) {
            const int chunk = std::min(batch_cap, n - i);

            if (static_cast<int>(kvPos) + chunk >= n_ctx - max_gen_tokens - 4) {
                // Context full - clear and signal caller to retry with full prompt.
                resetKV();
                return false;
            }

            llama_batch batch = llama_batch_init(chunk, 0, 1);
            for (int j = 0; j < chunk; ++j) {
                batch.token   [j] = tokens[static_cast<size_t>(i + j)];
                batch.pos     [j] = kvPos + j;
                batch.n_seq_id[j] = 1;
                batch.seq_id  [j][0] = 0;
                batch.logits  [j] = (compute_last_logit && (i + j == n - 1)) ? 1 : 0;
            }
            batch.n_tokens = chunk;

            const int rc = llama_decode(ctx, batch);
            llama_batch_free(batch);
            if (rc != 0) return false;

            kvPos += static_cast<llama_pos>(chunk);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
LlamaCppTransport::LlamaCppTransport(const llm::LLMConfig& config)
    : LLMTransport(llm::LLMProvider::LLAMA_CPP, config)
    , impl_(std::make_unique<Impl>()) {
}

LlamaCppTransport::~LlamaCppTransport() {
    // Stop the processing thread *before* impl_ (unique_ptr) is destroyed,
    // because the thread calls sendLLMRequest() which references impl_.
    stop();
    // impl_ destructor handles llama_sampler_free / llama_free / llama_model_free
    // / llama_backend_free automatically.
}

// ---------------------------------------------------------------------------
// initialize - override skips the HTTP-oriented apiKey/endpoint validation
// ---------------------------------------------------------------------------
bool LlamaCppTransport::initialize() {
    if (_initialized) {
        return true;
    }

    if (_config.model.empty()) {
        spdlog::error("[LlamaCppTransport] Model path not provided "
                      "(set LLMConfig::model to the GGUF file path)");
        if (onError) onError(1, "GGUF model path not provided in LLMConfig::model");
        return false;
    }

    // Redirect llama.cpp's internal log output through spdlog so errors appear
    // in Android logcat (by default they go to stderr which is not visible).
    llama_log_set([](ggml_log_level level, const char* text, void*) {
        if (!text || text[0] == '\0') return;
        // Strip trailing newline that llama.cpp always appends
        std::string msg(text);
        if (!msg.empty() && msg.back() == '\n') msg.pop_back();
        if (msg.empty()) return;
        switch (level) {
            case GGML_LOG_LEVEL_ERROR: spdlog::error("[llama.cpp] {}", msg); break;
            case GGML_LOG_LEVEL_WARN:  spdlog::warn ("[llama.cpp] {}", msg); break;
            default:                   spdlog::info ("[llama.cpp] {}", msg); break;
        }
    }, nullptr);

    // Register all backends compiled into this binary (CPU always present;
    // Vulkan/OpenCL/etc. included if built with those flags).
    ggml_backend_load_all();

    // Global llama.cpp backend init (idempotent; call once per process).
    llama_backend_init();
    impl_->backendInited = true;

    // ----- Pre-flight: verify the GGUF file exists and is non-empty --------
    struct stat st{};
    if (::stat(_config.model.c_str(), &st) != 0) {
        spdlog::error("[LlamaCppTransport] Model file not found: {} (errno={})",
                      _config.model, errno);
        if (onError) onError(2, "GGUF model file not found: " + _config.model);
        impl_->destroy();
        return false;
    }
    spdlog::info("[LlamaCppTransport] Model file found ({} MB): {}",
                 st.st_size / (1024 * 1024), _config.model);

    // ----- Load model -------------------------------------------------------
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = _config.nGpuLayers;
    impl_->model = llama_model_load_from_file(_config.model.c_str(), mparams);
    if (!impl_->model) {
        spdlog::error("[LlamaCppTransport] llama_model_load_from_file returned null for: {}",
                      _config.model);
        if (onError) onError(2, "Failed to load GGUF model from: " + _config.model);
        impl_->destroy();
        return false;
    }

    // ----- Create inference context -----------------------------------------
    // Choose thread count: user-configured, or (CPU cores - 2), clamped [2, 8].
    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    const int n_threads  = _config.nThreads > 0
                               ? _config.nThreads
                               : std::max(2, std::min(8, hw_threads - 2));
    const int n_batch    = _config.nBatch > 0 ? _config.nBatch : 512;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx          = static_cast<uint32_t>(_config.nCtx > 0 ? _config.nCtx : 4096);
    cparams.n_batch        = static_cast<uint32_t>(n_batch);
    cparams.n_ubatch       = static_cast<uint32_t>(n_batch);
    cparams.n_threads      = static_cast<uint32_t>(n_threads);
    cparams.n_threads_batch = static_cast<uint32_t>(n_threads);

    impl_->ctx = llama_init_from_model(impl_->model, cparams);
    if (!impl_->ctx) {
        spdlog::error("[LlamaCppTransport] Failed to create inference context");
        if (onError) onError(3, "Failed to create llama.cpp inference context");
        impl_->destroy();
        return false;
    }

    // ----- Build sampler chain -----------------------------------------------
    // Order matters: temperature → top-K → top-P → min-P → repetition → dist
    auto sparams   = llama_sampler_chain_default_params();
    impl_->sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(impl_->sampler,
                            llama_sampler_init_temp(_config.temperature > 0.f
                                                    ? _config.temperature
                                                    : 0.7f));
    if (_config.topK > 0) {
        llama_sampler_chain_add(impl_->sampler,
                                llama_sampler_init_top_k(_config.topK));
    }
    if (_config.topP < 1.0f && _config.topP > 0.0f) {
        llama_sampler_chain_add(impl_->sampler,
                                llama_sampler_init_top_p(_config.topP, 1));
    }
    if (_config.minP > 0.0f) {
        llama_sampler_chain_add(impl_->sampler,
                                llama_sampler_init_min_p(_config.minP, 1));
    }
    if (_config.repetitionPenalty > 1.0f) {
        llama_sampler_chain_add(impl_->sampler,
                                llama_sampler_init_penalties(64,
                                                             _config.repetitionPenalty,
                                                             0.0f, 0.0f));
    }
    llama_sampler_chain_add(impl_->sampler,
                            llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    char desc[128] = {};
    llama_model_desc(impl_->model, desc, sizeof(desc));
    const char* jinja = llama_model_chat_template(impl_->model, nullptr);
    if (jinja) spdlog::debug("[LlamaCppTransport] Chat template:\n{}", jinja);
    spdlog::info("[LlamaCppTransport] Loaded model \"{}\" | n_ctx={} | n_threads={} | n_batch={} | temp={} topK={} topP={} minP={} repPenalty={}",
                 desc, cparams.n_ctx, n_threads, n_batch,
                 _config.temperature, _config.topK, _config.topP, _config.minP, _config.repetitionPenalty);

    _initialized = true;
    setState(TransportState::DISCONNECTED);
    return true;
}

// ---------------------------------------------------------------------------
// clearHistory - clear conversation history and reset KV cache state
// ---------------------------------------------------------------------------
void LlamaCppTransport::clearHistory() {
    LLMTransport::clearHistory();
    if (impl_) impl_->resetKV();
}

// ---------------------------------------------------------------------------
// addMessageToHistory - delegate to base, then reset KV if trimming occurred
// ---------------------------------------------------------------------------
void LlamaCppTransport::addMessageToHistory(llm::MessageRole role, const std::string& content) {
    // Snapshot history size before the base-class add+trim logic runs.
    const size_t sizeBefore = getHistory().size();
    LLMTransport::addMessageToHistory(role, content);
    const size_t sizeAfter  = getHistory().size();

    // If the base class removed an entry (max-length trim), cachedFormattedLen
    // no longer matches the current history.  Reset the KV cache so the next
    // sendLLMRequest() re-encodes from scratch rather than computing a stale
    // delta and hitting the "KV cache state inconsistent" warning.
    //
    // +1 below: adding one message that wasn't trimmed means after == before+1.
    // If after <= before (one evicted, one added) a trim occurred.
    if (sizeAfter <= sizeBefore && impl_) {
        spdlog::debug("[LlamaCppTransport] History trimmed ({} → {}) - resetting KV cache",
                      sizeBefore, sizeAfter);
        impl_->resetKV();
    }
}

// ---------------------------------------------------------------------------
// cancel - interrupt an in-progress generation (e.g. on barge-in)
// ---------------------------------------------------------------------------
void LlamaCppTransport::cancel() {
    if (impl_) {
        impl_->cancelRequested.store(true, std::memory_order_relaxed);
        spdlog::info("[LlamaCppTransport] cancel() requested");
    }
}

// ---------------------------------------------------------------------------
// release - free llama resources then delegate to base for thread/history
// ---------------------------------------------------------------------------
void LlamaCppTransport::release() {
    LLMTransport::release(); // stop() + clearHistory()
    impl_->destroy();        // idempotent: safe even if already destroyed
    impl_->kvPos              = 0;
    impl_->cachedFormattedLen = 0;
    _initialized = false;
}

// ---------------------------------------------------------------------------
// stripToolCallsForHistory - remove tool-call fields from a JSON-format
// response before it is stored as the assistant history turn.
//
// The JSON tool-call format would otherwise store the raw response including
// its "tools" array. On the follow-up request the model then sees
//   assistant: {..., "tools": [...]}  →  tool: {result}
// and a small quantized model pattern-matches that structure, re-emitting the
// same tool call instead of answering - a self-imitation loop. Keeping only
// the "text" field breaks the pattern. (The LFM native path instead keeps the
// tool call in history and relies on the _forcingFinalAnswer token ban, which
// JSON formats lack - they have no tool-call marker token to ban.)
// Returns the response unchanged when it
// carries no tool call or cannot be parsed; `stripped` reports whether the
// stored turn now differs from the generated tokens.
// ---------------------------------------------------------------------------
namespace {
std::string stripToolCallsForHistory(const std::string& response, bool& stripped) {
    stripped = false;
    const size_t jsonStart = response.find('{');
    if (jsonStart == std::string::npos) return response;
    size_t depth = 0, jsonEnd = std::string::npos;
    for (size_t i = jsonStart; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        else if (response[i] == '}') {
            if (--depth == 0) { jsonEnd = i; break; }
        }
    }
    if (jsonEnd == std::string::npos) return response;

    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(response.substr(jsonStart, jsonEnd - jsonStart + 1));
    } catch (...) {
        return response;
    }
    if (!obj.is_object()) return response;

    const bool hasToolsArray = obj.contains("tools") && obj["tools"].is_array() &&
                               !obj["tools"].empty();
    const bool hasLegacyTool = obj.contains("tool");
    if (!hasToolsArray && !hasLegacyTool) return response;

    obj.erase("tools");
    obj.erase("tool");
    obj.erase("args");
    stripped = true;
    return obj.dump();
}
} // namespace

// ---------------------------------------------------------------------------
// sendLLMRequest - the core override: builds prompt, tokenises, generates
//
// Design: the base class LLMTransport::processIncomingFrame adds the user
// message to _conversationHistory BEFORE calling this function.  So
// _conversationHistory already contains the complete conversation including
// the new user turn when we arrive here.
//
// We track cachedFormattedLen = the byte length of the chat-formatted string
// that was decoded into the KV cache at the end of the previous turn:
//   format(prev_full_history, add_ass=false).size()
//
// Each turn we:
//   1. format(full_history_with_new_user_msg, add_ass=true)  → fullFormatted
//   2. newPart = fullFormatted[cachedFormattedLen:]           (just the delta)
//   3. tokenise + decode newPart  (incremental - O(new_tokens), not O(total))
//   4. generate
//   5. add assistant to history
//   6. cachedFormattedLen = format(now_complete_history, add_ass=false).size()
// ---------------------------------------------------------------------------
std::string LlamaCppTransport::sendLLMRequest(const std::string& userMessage) {
    // Any early-return failure path leaves _conversationHistory with a dangling USER entry
    // (added by processIncomingFrame before this is called) but no ASSISTANT reply.
    // Two consecutive USER messages cause the model to emit EOG immediately on the next
    // request, cascading into permanent 0-char responses.  This lambda removes the
    // dangling entry at every failure exit so history stays well-formed.
    // It is intentionally NOT called on the success path (where addMessageToHistory(ASSISTANT)
    // has already run and history ends with ASSISTANT).
    auto rollbackUserMessage = [&]() {
        std::lock_guard<std::mutex> lock(_historyMutex);
        if (!_conversationHistory.empty() &&
            _conversationHistory.back().role == llm::MessageRole::USER) {
            _conversationHistory.pop_back();
        }
    };

    if (!impl_->model || !impl_->ctx) {
        spdlog::error("[LlamaCppTransport] sendLLMRequest called but model is not loaded");
        if (onError) onError(4, "llama.cpp model not initialized");
        rollbackUserMessage();
        return "";
    }

    const llama_vocab* vocab = llama_model_get_vocab(impl_->model);
    const int          n_ctx = static_cast<int>(llama_n_ctx(impl_->ctx));
    const char*        tmpl  = llama_model_chat_template(impl_->model, nullptr);

    // -------------------------------------------------------------------------
    // 1. Build llama_chat_message list from the full conversation history.
    //    _conversationHistory already includes the new user message.
    //    Reusable so the overflow-retry path below can rebuild it after
    //    trimming history down without duplicating the role/content plumbing.
    // -------------------------------------------------------------------------
    auto formatFullHistory = [&]() -> std::string {
        std::vector<std::string>        roleStore, contentStore;
        std::vector<llama_chat_message> msgs;
        {
            std::lock_guard<std::mutex> lock(_historyMutex);
            roleStore.reserve(_conversationHistory.size());
            contentStore.reserve(_conversationHistory.size());
            msgs.reserve(_conversationHistory.size());
            for (const auto& msg : _conversationHistory) {
                switch (msg.role) {
                    case llm::MessageRole::SYSTEM:    roleStore.push_back("system");    break;
                    case llm::MessageRole::USER:      roleStore.push_back("user");      break;
                    case llm::MessageRole::ASSISTANT: roleStore.push_back("assistant"); break;
                    case llm::MessageRole::TOOL:      roleStore.push_back("tool");      break;
                }
                contentStore.push_back(msg.content);
                msgs.push_back({ roleStore.back().c_str(), contentStore.back().c_str() });
            }
        }
        return impl_->applyTemplate(tmpl, msgs, true);
    };

    // -------------------------------------------------------------------------
    // 2. Format the full history with add_ass=true to open the assistant turn.
    // -------------------------------------------------------------------------
    const std::string fullFormatted = formatFullHistory();
    if (fullFormatted.empty()) {
        spdlog::error("[LlamaCppTransport] llama_chat_apply_template failed");
        if (onError) onError(5, "Failed to apply chat template");
        rollbackUserMessage();
        return "";
    }

    // Guard: if state got out of sync (e.g. external clearHistory call), reset.
    if (impl_->cachedFormattedLen > fullFormatted.size()) {
        spdlog::warn("[LlamaCppTransport] KV cache state inconsistent - resetting");
        impl_->resetKV();
    }

    // -------------------------------------------------------------------------
    // 3. The new suffix is everything beyond what's already decoded.
    // -------------------------------------------------------------------------
    const bool        isFirstEver = (impl_->cachedFormattedLen == 0);
    if (isFirstEver) {
        spdlog::debug("[LlamaCppTransport] Formatted prompt (full, {} chars):\n{}",
                      fullFormatted.size(), fullFormatted);
    }
    const std::string newPart     = isFirstEver
                                        ? fullFormatted
                                        : fullFormatted.substr(impl_->cachedFormattedLen);

    if (newPart.empty()) {
        spdlog::warn("[LlamaCppTransport] Nothing new to encode - skipping");
        rollbackUserMessage();
        return "";
    }

    auto newTokens = Impl::tokenize(vocab, newPart, isFirstEver, true);
    if (newTokens.empty()) {
        spdlog::error("[LlamaCppTransport] Tokenisation produced empty result");
        if (onError) onError(6, "Prompt tokenisation failed");
        rollbackUserMessage();
        return "";
    }

    // Save rollback point so barge-in cancel can undo this turn's KV writes.
    const llama_pos savedKvPos          = impl_->kvPos;
    const size_t    savedFormattedLen   = impl_->cachedFormattedLen;

    // -------------------------------------------------------------------------
    // 4. Decode the new tokens.  On context overflow decodeTokens resets the KV
    //    cache and returns false; retry by re-encoding the full prompt.
    // -------------------------------------------------------------------------
    llama_sampler_reset(impl_->sampler);

    if (!impl_->decodeTokens(newTokens, n_ctx, _config.maxTokens, true)) {
        spdlog::warn("[LlamaCppTransport] Prefill overflow - re-encoding full prompt");
        // impl_->resetKV() was already called inside decodeTokens
        std::string reencoded = fullFormatted;
        auto        fullTokens = Impl::tokenize(vocab, reencoded, true, true);
        bool        decoded    = impl_->decodeTokens(fullTokens, n_ctx, _config.maxTokens, true);

        // Even the full re-encode doesn't fit - a single oversized history
        // entry (e.g. a big get_contacts/get_installed_apps tool result) can
        // blow the budget well before _maxHistoryLength (a message *count*
        // cap) kicks in. Drop the oldest non-system messages one at a time
        // and retry rather than failing the turn outright.
        while (!decoded) {
            bool trimmed = false;
            {
                std::lock_guard<std::mutex> lock(_historyMutex);
                for (auto it = _conversationHistory.begin(); it != _conversationHistory.end(); ++it) {
                    const bool isLast = (&*it == &_conversationHistory.back());
                    if (it->role != llm::MessageRole::SYSTEM && !isLast) {
                        _conversationHistory.erase(it);
                        trimmed = true;
                        break;
                    }
                }
            }
            if (!trimmed) break; // nothing left to drop but system + latest turn
            spdlog::warn("[LlamaCppTransport] Still over budget after re-encode - "
                         "dropped oldest history entry and retrying");
            impl_->resetKV();
            reencoded  = formatFullHistory();
            fullTokens = Impl::tokenize(vocab, reencoded, true, true);
            decoded    = impl_->decodeTokens(fullTokens, n_ctx, _config.maxTokens, true);
        }

        if (!decoded) {
            spdlog::error("[LlamaCppTransport] Prefill decode failed");
            if (onError) onError(7, "Prompt decode failed");
            impl_->resetKV(); // ensure no partial decode state leaks into next request
            rollbackUserMessage();
            return "";
        }
    }

    // -------------------------------------------------------------------------
    // 5. Generation loop.
    // -------------------------------------------------------------------------
    // Some formats (LFM2/LFM2.5) wrap native tool calls in start/end marker
    // tokens (see step 7 below) - sourced from the active PromptTemplate rather
    // than hardcoded so this stays correct if a format's markers ever change or
    // a new format adopts the same convention. Empty for formats with no such
    // wrapper (JSON tool calls have no markers to watch for).
    const auto [kToolCallStart, kToolCallEnd] =
        llm::PromptTemplate::create(getPromptChatFormat())->toolCallTags();
    const bool hasToolCallTags = !kToolCallStart.empty();

    // The mid-gen chunker below has no idea what a tool call looks like - it
    // just splits on punctuation for low-latency TTS streaming - so once the
    // start marker shows up in the raw output, every remaining token is part
    // of the tool-call block and must NOT be chunked/spoken; only step 7
    // (after generation completes) knows how to handle it correctly.
    bool toolCallStarted = false;

    // On the post-tool-result follow-up the model must answer, not re-emit the
    // tool call it just made - prompt-level nudges are unreliable on small
    // quantized models (they echo the nudge or call the tool again, executing
    // it twice), so ban the tool-call start token outright. Only a marker that
    // tokenises to a single special token can be banned this way; banning the
    // first token of a multi-token marker could suppress ordinary text.
    llama_token bannedToolCallToken = -1;
    const bool forcingFinalAnswer = _forcingFinalAnswer.load(std::memory_order_relaxed);
    if (forcingFinalAnswer && hasToolCallTags) {
        const auto markerTokens = Impl::tokenize(vocab, kToolCallStart, false, true);
        if (markerTokens.size() == 1) {
            bannedToolCallToken = markerTokens[0];
        } else {
            spdlog::warn("[LlamaCppTransport] Tool-call start marker '{}' is not a "
                         "single token ({}) - cannot suppress tool calls this turn",
                         kToolCallStart, markerTokens.size());
        }
    }

    std::string response;
    response.reserve(512);

    impl_->cancelRequested.store(false, std::memory_order_relaxed);

    char   piece[256];
    size_t lastPartialEnd    = 0;
    int    sentenceChunkCount = 0;

    for (int i = 0; i < _config.maxTokens; ++i) {
        if (impl_->cancelRequested.load(std::memory_order_relaxed)) {
            spdlog::info("[LlamaCppTransport] Generation cancelled after {} tokens", i);
            // Roll back KV cache to before this turn so next call re-encodes cleanly.
            llama_memory_seq_rm(llama_get_memory(impl_->ctx), 0,
                                savedKvPos, impl_->kvPos);
            impl_->kvPos              = savedKvPos;
            impl_->cachedFormattedLen = savedFormattedLen;
            // Also remove the user message that processIncomingFrame added to history
            // before calling sendLLMRequest.  Without this, the next request would see
            // two consecutive USER messages (cancelled turn + new turn) with no ASSISTANT
            // between them, causing the model to emit EOG immediately (0-char response).
            {
                std::lock_guard<std::mutex> lock(_historyMutex);
                if (!_conversationHistory.empty() &&
                    _conversationHistory.back().role == llm::MessageRole::USER) {
                    _conversationHistory.pop_back();
                }
            }
            // Reset sampler so stale greedy/penalty state from this partial run
            // does not carry over and cause an immediate EOG on the next request.
            llama_sampler_reset(impl_->sampler);
            return "";
        }

        if (bannedToolCallToken >= 0) {
            llama_get_logits_ith(impl_->ctx, -1)[bannedToolCallToken] =
                -std::numeric_limits<float>::infinity();
        }
        llama_token token = llama_sampler_sample(impl_->sampler, impl_->ctx, -1);

        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }

        int n_piece = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
        if (n_piece > 0) {
            const size_t newTokenStart = response.size();
            response.append(piece, static_cast<size_t>(n_piece));

            if (hasToolCallTags && !toolCallStarted &&
                response.find(kToolCallStart) != std::string::npos) {
                toolCallStarted = true;
            }

            if (!toolCallStarted) {
                const size_t chunkEnd = chunker_->findChunkEnd(response, lastPartialEnd, newTokenStart);
                if (chunkEnd != std::string::npos) {
                    auto partialFrame = std::make_shared<TextFrame>();
                    partialFrame->text       = response.substr(lastPartialEnd, chunkEnd - lastPartialEnd);
                    partialFrame->timestamp  = Frame::getCurrentTimestamp();
                    partialFrame->sequenceId = _nextSequenceId++;
                    partialFrame->sourceId   = getName();
                    partialFrame->isFinal    = false;
                    fireOnFrameReceived(partialFrame);
                    lastPartialEnd = chunkEnd;
                    ++sentenceChunkCount;
                }
            }
        }

        // Feed the sampled token back for the next decoding step.
        llama_batch next = llama_batch_init(1, 0, 1);
        next.token   [0] = token;
        next.pos     [0] = impl_->kvPos;
        next.n_seq_id[0] = 1;
        next.seq_id  [0][0] = 0;
        next.logits  [0] = 1;
        next.n_tokens    = 1;
        const int rc = llama_decode(impl_->ctx, next);
        llama_batch_free(next);
        if (rc != 0) {
            spdlog::warn("[LlamaCppTransport] Decode failed during generation - stopping early");
            break;
        }
        impl_->kvPos++;
    }

    spdlog::info("[LlamaCppTransport] Generated \"{}\" {} chars ({} mid-gen chunks, chunker: {}) | prompt: \"{}{}\"",
                 response, response.size(), sentenceChunkCount, chunker_->name(),
                 userMessage.substr(0, std::min<size_t>(40, userMessage.size())),
                 userMessage.size() > 40 ? "..." : "");

    if (response.empty()) {
        // The model emitted EOG on the very first token - no output was generated.
        // The prefill decode (decodeTokens) already ran and advanced kvPos past the
        // new user turn's tokens.  If we only remove the user message from history
        // without also rolling back kvPos, the stale prefill tokens remain in the KV
        // cache at the wrong positions and corrupt every subsequent turn's context
        // (causing garbled short responses or repeated immediate-EOG 0-char outputs).
        llama_memory_seq_rm(llama_get_memory(impl_->ctx), 0,
                            savedKvPos, impl_->kvPos);
        impl_->kvPos              = savedKvPos;
        impl_->cachedFormattedLen = savedFormattedLen;
        llama_sampler_reset(impl_->sampler);
        rollbackUserMessage();
        return "";
    }

    // -------------------------------------------------------------------------
    // 7. LFM tool-call detection.
    //    LFM wraps tool calls in <|tool_call_start|>…<|tool_call_end|>.
    //    When present, emit any speech text (before OR after the markers) as a
    //    final TextFrame, parse the JSON array, and emit one ToolCallFrame per
    //    tool. The normal tail-frame path is skipped entirely.
    //    (kToolCallStart/kToolCallEnd declared above, before the generation loop.)
    // -------------------------------------------------------------------------

    const size_t tcStart = hasToolCallTags ? response.find(kToolCallStart) : std::string::npos;
    if (tcStart != std::string::npos) {
        const size_t tcEnd = response.find(kToolCallEnd, tcStart);
        if (tcEnd != std::string::npos) {
            // Collect any speech text surrounding the tool call markers.
            const std::string preSpeech  = response.substr(0, tcStart);
            const size_t      afterEnd   = tcEnd + kToolCallEnd.size();
            const std::string postSpeech = afterEnd < response.size()
                                               ? response.substr(afterEnd)
                                               : std::string{};
            // Trim leading whitespace from post-call speech.
            const size_t postTrimmed = postSpeech.find_first_not_of(" \t\n\r");
            std::string speechPart = !preSpeech.empty()
                                         ? preSpeech
                                         : (postTrimmed != std::string::npos
                                                ? postSpeech.substr(postTrimmed)
                                                : std::string{});
            // A degenerate generation can spill extra tool-call fragments past
            // the first end marker (e.g. "[tool()]<|tool_call_end|>"); anything
            // still containing a marker is not speech - drop it rather than
            // handing it to TTS to be read aloud.
            if (speechPart.find(kToolCallStart) != std::string::npos ||
                speechPart.find(kToolCallEnd)   != std::string::npos) {
                spdlog::warn("[LlamaCppTransport] Dropping malformed speech text "
                             "around tool call: \"{}\"", speechPart);
                speechPart.clear();
            }
            if (!speechPart.empty()) {
                auto speechFrame = std::make_shared<transport::TextFrame>();
                speechFrame->text       = speechPart;
                speechFrame->timestamp  = transport::Frame::getCurrentTimestamp();
                speechFrame->sequenceId = _nextSequenceId++;
                speechFrame->sourceId   = getName();
                speechFrame->isFinal    = true;
                fireOnFrameReceived(speechFrame);
            }

            // Backstop for when the token ban above couldn't be applied: never
            // dispatch a tool call generated on the post-tool-result follow-up.
            // Re-dispatching would run the tool a second time (toggling
            // hardware back) and loop forever - there is no loop guard
            // downstream in ToolDispatchProcessor.
            if (forcingFinalAnswer) {
                spdlog::warn("[LlamaCppTransport] Model re-emitted a tool call on the "
                             "post-tool-result turn - dropping it: \"{}\"", response);
                addMessageToHistory(llm::MessageRole::ASSISTANT, "");
                impl_->resetKV();
                return "";
            }

            // Parse the JSON array between the tokens.
            const size_t jsonStart = tcStart + kToolCallStart.size();
            const std::string toolJson = response.substr(jsonStart, tcEnd - jsonStart);
            try {
                auto arr = nlohmann::json::parse(toolJson);
                if (arr.is_array()) {
                    for (const auto& call : arr) {
                        const std::string name = call.value("name", "");
                        if (name.empty()) continue;
                        const nlohmann::json& argsNode = call.contains("arguments")
                                                             ? call["arguments"]
                                                             : nlohmann::json::object();
                        auto toolFrame        = std::make_shared<transport::ToolCallFrame>();
                        toolFrame->toolName   = name;
                        toolFrame->argsJson   = argsNode.is_string()
                                                    ? argsNode.get<std::string>()
                                                    : argsNode.dump();
                        toolFrame->timestamp  = transport::Frame::getCurrentTimestamp();
                        toolFrame->sequenceId = _nextSequenceId++;
                        toolFrame->sourceId   = getName();
                        spdlog::info("[LlamaCppTransport] Tool call: {} args={}", name, toolFrame->argsJson);
                        fireOnFrameReceived(toolFrame);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("[LlamaCppTransport] Failed to parse LFM tool call JSON: {}", e.what());
            }

            // Store the tool call itself (native tag format) as the assistant
            // history turn. A blank turn here hides every past tool call from
            // the model: history then shows action requests answered by plain
            // text, and on a later similar request a small model imitates that
            // and answers in text without calling the tool. Re-emission on the
            // immediate follow-up is already prevented at the token level
            // (_forcingFinalAnswer bans the start marker, with the drop
            // backstop above), so keeping the call visible is safe.
            // The speech text is sent to TTS but excluded from history.
            addMessageToHistory(llm::MessageRole::ASSISTANT,
                                kToolCallStart + toolJson + kToolCallEnd);
            // The KV cache contains the raw generated tokens (speech + tool-call markers),
            // but cachedFormattedLen would only cover the speech part via the chat template.
            // These are different byte sequences, so incremental decoding would misalign.
            // Reset KV to force a clean full re-encode when injectToolResults calls back.
            impl_->resetKV();
            return "";
        }
    }

    // Emit remaining tail text (isFinal=true marks the last chunk of this response).
    if (lastPartialEnd < response.size()) {
        auto tailFrame = std::make_shared<TextFrame>();
        tailFrame->text       = response.substr(lastPartialEnd);
        tailFrame->timestamp  = Frame::getCurrentTimestamp();
        tailFrame->sequenceId = _nextSequenceId++;
        tailFrame->sourceId   = getName();
        tailFrame->isFinal    = true;
        fireOnFrameReceived(tailFrame);
    } else if (lastPartialEnd == 0) {
        auto fullFrame = std::make_shared<TextFrame>();
        fullFrame->text       = response;
        fullFrame->timestamp  = Frame::getCurrentTimestamp();
        fullFrame->sequenceId = _nextSequenceId++;
        fullFrame->sourceId   = getName();
        fullFrame->isFinal    = true;
        fireOnFrameReceived(fullFrame);
    } else {
        // All text was emitted as mid-gen chunks; send an empty final marker.
        auto doneFrame = std::make_shared<TextFrame>();
        doneFrame->text       = "";
        doneFrame->timestamp  = Frame::getCurrentTimestamp();
        doneFrame->sequenceId = _nextSequenceId++;
        doneFrame->sourceId   = getName();
        doneFrame->isFinal    = true;
        fireOnFrameReceived(doneFrame);
    }

    // -------------------------------------------------------------------------
    // 6. Add assistant response to history, then record the new baseline for
    //    cachedFormattedLen: format the now-complete history WITHOUT add_ass.
    //    This gives us the exact byte offset the next turn's delta starts from.
    //    JSON-format tool calls are stripped from the stored turn (see
    //    stripToolCallsForHistory) so the model doesn't imitate them on the
    //    follow-up request.
    // -------------------------------------------------------------------------
    bool strippedToolCalls = false;
    addMessageToHistory(llm::MessageRole::ASSISTANT,
                        stripToolCallsForHistory(response, strippedToolCalls));

    if (strippedToolCalls) {
        // The KV cache holds this turn's raw generated tokens (including the
        // tools array), but history now stores the stripped turn - different
        // byte sequences, so incremental decoding would misalign. Reset to
        // force a clean full re-encode on the next request (same tradeoff the
        // LFM tool-call path above makes).
        impl_->resetKV();
        return "";
    }

    {
        std::vector<std::string>        rs2, cs2;
        std::vector<llama_chat_message> m2;
        {
            std::lock_guard<std::mutex> lock(_historyMutex);
            rs2.reserve(_conversationHistory.size());
            cs2.reserve(_conversationHistory.size());
            m2.reserve(_conversationHistory.size());
            for (const auto& msg : _conversationHistory) {
                switch (msg.role) {
                    case llm::MessageRole::SYSTEM:    rs2.push_back("system");    break;
                    case llm::MessageRole::USER:      rs2.push_back("user");      break;
                    case llm::MessageRole::ASSISTANT: rs2.push_back("assistant"); break;
                    case llm::MessageRole::TOOL:      rs2.push_back("tool");      break;
                }
                cs2.push_back(msg.content);
                m2.push_back({ rs2.back().c_str(), cs2.back().c_str() });
            }
        }
        const std::string afterAsst = impl_->applyTemplate(tmpl, m2, false);
        impl_->cachedFormattedLen = afterAsst.size();
    }

    // Return "" so LLMTransport::processIncomingFrame skips its own enqueue.
    return "";
}

} // namespace transport
} // namespace va
