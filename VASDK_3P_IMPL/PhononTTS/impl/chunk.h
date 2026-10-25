#pragma once
// chunk.h — splitting text into the pieces the model is prompted with: a C++ port of
// gradium-ai/xn-ptts v0.4.1 `ptts::plan` (chunks, fit, fit_or_error, frame_budget) and
// `ptts::tts_model::split_into_best_sentences`.
//
// The model learnt to say about 50 tokens of text in one go. Given much more it stops early
// and drops words, so a long reply is grouped into whole sentences of up to 50 tokens, and a
// single sentence over 200 tokens is cut at the clause or word nearest its middle. Each piece
// is then generated on its own, from the voice-primed state. tests/chunk_test.cpp checks the
// port against the reference: the Rust unit tests, and golden files the reference produced
// with the checkpoint's own tokenizer.

#include "normalize.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gradium::tts::text {

    // The tokenizer the chunker measures text with: the checkpoint's SentencePiece model in
    // the engine, a mock one in the tests.
    class Tokenizer {
        public:
        virtual ~Tokenizer() = default;
        virtual std::vector<int> encode(std::string_view text) const = 0;
        virtual std::string decode(const std::vector<int>& tokens) const = 0;
    };

    // Default sentence-group size (tts_model.rs MAX_TOKENS_PER_CHUNK).
    constexpr size_t kMaxTokensPerChunk = 50;
    // Largest chunk left after cutting an oversized sentence (plan.rs MAX_FIT_TOKENS).
    constexpr size_t kMaxFitTokens = 200;

    // Frames to generate at most for `numTokens` text tokens: about three tokens per second
    // of speech plus two seconds, at the codec frame rate. Generation normally ends earlier,
    // at the model's end-of-speech signal.
    size_t frameBudget(size_t numTokens, double frameRate);

    // The largest chunk, in tokens, that fits `room` KV slots with its frame budget.
    size_t maxTokensFor(size_t room, double frameRate);

    // One piece of text, ready to prompt the model with.
    struct Chunk {
        std::string text;          // the normalized piece, before prepareTextPrompt
        std::vector<int> tokens;   // prepareTextPrompt(text), tokenized
        int framesAfterEos = 3;
        size_t frameBudget = 0;
    };

    // Prepare and tokenize one piece of already normalized text.
    Chunk makeChunk(std::string text, const Tokenizer& tok, double frameRate);

    // Whole sentences of `text`, after prepareTextPrompt, grouped greedily into pieces of at
    // most `maxTokens` tokens. A sentence ends after a run of ".", "!", "..." or "?" tokens;
    // one sentence longer than `maxTokens` stays a piece of its own.
    std::vector<std::string> splitIntoBestSentences(const Tokenizer& tok, std::string_view text,
                                                    size_t maxTokens = kMaxTokensPerChunk);

    // Normalize `text` (when `lang` is set), then split it into sentence groups of at most
    // `maxTokens` tokens. Throws std::invalid_argument when nothing is left to say.
    std::vector<Chunk> chunks(const Tokenizer& tok, std::string_view text, std::optional<Lang> lang,
                              Rules rules, size_t maxTokens, double frameRate);

    // Cut every chunk of more than `max` tokens until each piece fits, keeping their order: at
    // the sentence end nearest its middle, else after the comma, semicolon or colon nearest it,
    // else at the middle word, each side keeping a quarter of `max`. A single word over
    // `wordLimit` is cut between its characters.
    std::vector<Chunk> fit(std::vector<Chunk> chunks, size_t max, const Tokenizer& tok, double frameRate);
    std::vector<Chunk> fitWithWordLimit(std::vector<Chunk> chunks, size_t max, size_t wordLimit,
                                        const Tokenizer& tok, double frameRate);
    // fit(), then std::invalid_argument if a piece (a single oversized character) still exceeds `max`.
    std::vector<Chunk> fitOrError(std::vector<Chunk> chunks, size_t max, const Tokenizer& tok, double frameRate);

    // What Gradium's runtime (ptts::synth) does with a request: chunks() at kMaxTokensPerChunk,
    // then fitOrError() at kMaxFitTokens.
    std::vector<Chunk> planChunks(const Tokenizer& tok, std::string_view text, std::optional<Lang> lang,
                                  Rules rules, double frameRate, size_t maxTokens = kMaxTokensPerChunk,
                                  std::optional<size_t> fitTo = kMaxFitTokens);

} // namespace gradium::tts::text
