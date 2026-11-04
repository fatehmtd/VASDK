#include "LlamaCppCompletion.h"

#include <llama.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace va {
namespace transport {

struct LlamaCppCompletion::Impl {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    llama_sampler* sampler = nullptr;

    ~Impl() {
        if (sampler) llama_sampler_free(sampler);
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
        // llama_backend_free is left out: a LlamaCppTransport may still be using the backend.
    }
};

LlamaCppCompletion::LlamaCppCompletion(llm::LLMConfig config) : _config(std::move(config)), _impl(std::make_unique<Impl>()) {
    if (_config.model.empty()) {
        spdlog::error("[LlamaCppCompletion] no model: set LLMConfig::model to the GGUF file's path");
        return;
    }
    ggml_backend_load_all();
    llama_backend_init();
    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = _config.nGpuLayers;
    _impl->model = llama_model_load_from_file(_config.model.c_str(), modelParams);
    if (!_impl->model) {
        spdlog::error("[LlamaCppCompletion] can't load {}", _config.model);
        return;
    }
    const int hardwareThreads = static_cast<int>(std::thread::hardware_concurrency());
    const int threads = _config.nThreads > 0 ? _config.nThreads : std::max(2, std::min(8, hardwareThreads - 2));
    const int batch = _config.nBatch > 0 ? _config.nBatch : 512;
    llama_context_params contextParams = llama_context_default_params();
    contextParams.n_ctx = static_cast<uint32_t>(_config.nCtx > 0 ? _config.nCtx : 4096);
    contextParams.n_batch = static_cast<uint32_t>(batch);
    contextParams.n_ubatch = static_cast<uint32_t>(batch);
    contextParams.n_threads = static_cast<uint32_t>(threads);
    contextParams.n_threads_batch = static_cast<uint32_t>(threads);
    _impl->ctx = llama_init_from_model(_impl->model, contextParams);
    if (!_impl->ctx) {
        spdlog::error("[LlamaCppCompletion] can't create a context for {}", _config.model);
        llama_model_free(_impl->model);
        _impl->model = nullptr;
        return;
    }
    _impl->sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (_config.temperature > 0.0f) {
        llama_sampler_chain_add(_impl->sampler, llama_sampler_init_temp(_config.temperature));
        llama_sampler_chain_add(_impl->sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    } else {
        llama_sampler_chain_add(_impl->sampler, llama_sampler_init_greedy());
    }
    spdlog::info("[LlamaCppCompletion] {} loaded (n_ctx {}, {} threads)", _config.model, contextParams.n_ctx, threads);
}

LlamaCppCompletion::~LlamaCppCompletion() = default;

bool LlamaCppCompletion::loaded() const { return _impl->ctx != nullptr; }

std::optional<std::string> LlamaCppCompletion::complete(const std::vector<llm::ConversationMessage>& messages) const {
    return completeWithin(messages, 0);
}

std::optional<std::string> LlamaCppCompletion::completeWithin(const std::vector<llm::ConversationMessage>& messages,
                                                              int timeLimitMs) const {
    if (!loaded() || messages.empty()) return std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    auto pastLimit = [&] {
        return timeLimitMs > 0 && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeLimitMs);
    };
    std::lock_guard<std::mutex> lock(_mutex);
    if (pastLimit()) return std::nullopt;   // waited for another question

    // The conversation in the model's own chat format, the assistant's turn opened.
    std::vector<std::string> roles;
    roles.reserve(messages.size());
    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    for (const auto& message : messages) {
        roles.push_back(message.role == llm::MessageRole::SYSTEM      ? "system"
                        : message.role == llm::MessageRole::ASSISTANT ? "assistant"
                        : message.role == llm::MessageRole::TOOL      ? "tool"
                                                                      : "user");
        chat.push_back({roles.back().c_str(), message.content.c_str()});
    }
    const char* chatTemplate = llama_model_chat_template(_impl->model, nullptr);
    std::vector<char> formatted(8192);
    int length = llama_chat_apply_template(chatTemplate, chat.data(), chat.size(), true, formatted.data(),
                                           static_cast<int32_t>(formatted.size()));
    if (length > static_cast<int>(formatted.size())) {
        formatted.resize(static_cast<size_t>(length) + 1);
        length = llama_chat_apply_template(chatTemplate, chat.data(), chat.size(), true, formatted.data(),
                                           static_cast<int32_t>(formatted.size()));
    }
    if (length <= 0) {
        spdlog::warn("[LlamaCppCompletion] the model's chat template failed");
        return std::nullopt;
    }
    const std::string prompt(formatted.data(), static_cast<size_t>(length));

    const llama_vocab* vocab = llama_model_get_vocab(_impl->model);
    const int needed = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    std::vector<llama_token> tokens(static_cast<size_t>(std::max(needed, 0)));
    if (tokens.empty() || llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                                         static_cast<int32_t>(tokens.size()), true, true) < 0)
        return std::nullopt;
    const int maxTokens = _config.maxTokens > 0 ? _config.maxTokens : 400;
    const int contextSize = static_cast<int>(llama_n_ctx(_impl->ctx));
    if (static_cast<int>(tokens.size()) + maxTokens >= contextSize) {
        spdlog::warn("[LlamaCppCompletion] the question ({} tokens) and its answer don't fit in {} tokens", tokens.size(), contextSize);
        return std::nullopt;
    }

    // Each question starts from an empty context.
    llama_memory_clear(llama_get_memory(_impl->ctx), true);
    llama_sampler_reset(_impl->sampler);
    const int batchSize = static_cast<int>(llama_n_batch(_impl->ctx));
    for (size_t at = 0; at < tokens.size(); at += static_cast<size_t>(batchSize)) {
        const int count = std::min(batchSize, static_cast<int>(tokens.size() - at));
        if (llama_decode(_impl->ctx, llama_batch_get_one(tokens.data() + at, count)) != 0) {
            spdlog::warn("[LlamaCppCompletion] the prompt didn't decode");
            return std::nullopt;
        }
        if (pastLimit()) return std::nullopt;
    }

    std::string answer;
    char piece[256];
    for (int i = 0; i < maxTokens; ++i) {
        if (pastLimit()) {
            spdlog::info("[LlamaCppCompletion] no answer within {} ms", timeLimitMs);
            return std::nullopt;
        }
        llama_token token = llama_sampler_sample(_impl->sampler, _impl->ctx, -1);
        if (llama_vocab_is_eog(vocab, token)) break;
        const int size = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
        if (size > 0) answer.append(piece, static_cast<size_t>(size));
        if (llama_decode(_impl->ctx, llama_batch_get_one(&token, 1)) != 0) break;
    }
    if (answer.empty()) return std::nullopt;
    return answer;
}

} // namespace transport
} // namespace va
