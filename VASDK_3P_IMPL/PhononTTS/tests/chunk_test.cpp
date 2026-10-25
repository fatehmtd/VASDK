// Parity tests for the C++ port of the Phonon text planner (impl/chunk.cpp) against the
// reference, gradium-ai/xn-ptts v0.4.1 `ptts::plan` and `split_into_best_sentences`.
//
//   phonon_chunk_test [TOKENIZER.model GOLDEN.jsonl...]
//
// Without arguments, only the reference's unit tests of plan.rs run, ported below with their
// mock tokenizers. With the checkpoint's tokenizer.model, every line of GOLDEN.jsonl (made by
// tests/golden-gen with the checkpoint's tokenizer.json) is checked too:
//   {"kind": "tokens", "text", "expected": [ids]}       the tokenizer alone
//   {"kind": "chunks", "lang", "rules", "max_tokens", "fit", "text", "expected"}
//       plan::chunks(text, max_tokens), then plan::fit_or_error(fit) when fit is set: every
//       chunk's text, tokens, frames after EOS and frame budget, or {"error"}.
// A tokenizer path that does not exist (the checkpoint is gated and may not be downloaded)
// skips the golden check with exit code 77.

#include "chunk.h"

#include <nlohmann/json.hpp>
#include <sentencepiece_processor.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gradium::tts::text;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const std::string& what) {
    if (!(got == want)) {
        ++g_failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

constexpr double kRate = 12.5;

// plan.rs `Words`: one token per word and per ".", "!" or "?", so sentence ends get their
// own ids as they do in the real tokenizer.
class Words final : public Tokenizer {
    public:
    std::vector<int> encode(std::string_view text) const override {
        std::vector<int> ids;
        for (std::string_view word : detail::splitWhitespace(text)) {
            std::string_view stem = word;
            while (!stem.empty() && (stem.back() == '.' || stem.back() == '!' || stem.back() == '?'))
                stem.remove_suffix(1);
            if (!stem.empty()) ids.push_back(id(std::string(stem)));
            for (char c : word.substr(stem.size())) ids.push_back(id(std::string(1, c)));
        }
        return ids;
    }
    std::string decode(const std::vector<int>& tokens) const override {
        std::string text;
        for (int t : tokens) {
            const std::string& piece = _vocab[t];
            if (!text.empty() && piece != "." && piece != "!" && piece != "?") text += ' ';
            text += piece;
        }
        return text;
    }

    private:
    int id(const std::string& piece) const {
        for (size_t i = 0; i < _vocab.size(); ++i)
            if (_vocab[i] == piece) return static_cast<int>(i);
        _vocab.push_back(piece);
        return static_cast<int>(_vocab.size() - 1);
    }
    mutable std::vector<std::string> _vocab;
};

// plan.rs `Chars`: one token per character, so a single word can be longer than the limit.
class Chars final : public Tokenizer {
    public:
    // Ids are code points (the test inputs are valid UTF-8).
    std::vector<int> encode(std::string_view text) const override {
        std::vector<int> ids;
        for (size_t i = 0; i < text.size();) {
            const auto b = static_cast<unsigned char>(text[i]);
            const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
            int cp = n == 1 ? b : b & (0x7F >> n);
            for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (text[i + k] & 0x3F);
            ids.push_back(cp);
            i += n;
        }
        return ids;
    }
    std::string decode(const std::vector<int>& tokens) const override {
        std::string out;
        for (int cp : tokens) {
            if (cp < 0x80) {
                out += char(cp);
            } else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xF0 | (cp >> 18));
                out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }
};

std::vector<std::string> texts(const std::vector<Chunk>& chunks) {
    std::vector<std::string> out;
    for (const Chunk& c : chunks) out.push_back(c.text);
    return out;
}

// The pieces as the model reads them.
std::vector<std::string> prompts(const std::vector<Chunk>& chunks) {
    std::vector<std::string> out;
    for (const Chunk& c : chunks) out.push_back(prepareTextPrompt(c.text).text);
    return out;
}

using Strings = std::vector<std::string>;

std::vector<Chunk> plan(const Tokenizer& tok, std::string_view text, size_t max) {
    return chunks(tok, text, std::nullopt, Rules::none(), max, kRate);
}

std::vector<Chunk> fitText(const Tokenizer& tok, const std::string& text, size_t max) {
    return fit({makeChunk(text, tok, kRate)}, max, tok, kRate);
}

bool allFit(const std::vector<Chunk>& chunks, size_t max) {
    for (const Chunk& c : chunks)
        if (c.tokens.size() > max) return false;
    return true;
}

void testPlanRs() {
    {   // chunks_group_whole_sentences_up_to_the_limit
        Words tok;
        const char* text = "one two. three four. five six.";
        checkEq(texts(plan(tok, text, 8)), Strings{"One two. three four.", "five six."}, "group at 8");
        checkEq(texts(plan(tok, text, 9)), Strings{"One two. three four. five six."}, "group at 9");
    }
    {   // a_sentence_over_the_limit_stays_one_chunk
        Words tok;
        const auto c = plan(tok, "a b c d e f g h", 3);
        checkEq(texts(c), Strings{"A b c d e f g h."}, "long sentence stays whole");
        check(c[0].tokens.size() > 3, "long sentence is over the limit");
    }
    {   // each_chunk_is_prepared_and_budgeted_on_its_own
        Words tok;
        const auto c = plan(tok, "short one. and now a longer second sentence", 4);
        checkEq(c.size(), size_t(2), "two chunks");
        for (const Chunk& chunk : c) {
            const Prompt p = prepareTextPrompt(chunk.text);
            checkEq(chunk.tokens, tok.encode(p.text), "chunk tokens");
            checkEq(chunk.framesAfterEos, p.framesAfterEos, "chunk frames after eos");
            checkEq(chunk.frameBudget, frameBudget(chunk.tokens.size(), kRate), "chunk frame budget");
        }
    }
    {   // chunks_normalize_the_whole_text_first
        Words tok;
        const auto c = chunks(tok, "mail a@b now", Lang::En, Rules::defaults(), 50, kRate);
        checkEq(texts(c), Strings{"Mail a at b now."}, "normalized first");
    }
    {   // empty_text_is_an_error
        Words tok;
        bool threw = false;
        try { plan(tok, "   ", 50); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "empty text is an error");
    }
    {   // fit_cuts_after_the_middle_comma_without_a_full_stop
        Words tok;
        checkEq(prompts(fitText(tok, "one two three four five, six seven eight nine ten eleven twelve", 8)),
                Strings{"One two three four five,", "Six seven eight nine ten eleven twelve."}, "middle comma");
    }
    {   // fit_passes_over_a_comma_that_would_leave_a_tiny_piece
        Words tok;
        checkEq(prompts(fitText(tok, "one, two three four five six seven eight", 5)),
                Strings{"One, two three four.", "Five six seven eight."}, "tiny piece comma");
    }
    {   // fit_counts_a_comma_inside_closing_quotes
        Words tok;
        checkEq(prompts(fitText(tok, "one two three four \"five,\" six seven eight nine ten eleven twelve", 8)),
                Strings{"One two three four \"five,\"", "Six seven eight nine ten eleven twelve."}, "quoted comma");
    }
    {   // fit_prefers_a_sentence_end_to_a_comma
        Words tok;
        const auto planned = plan(tok, "one two, three four five six. seven eight nine ten.", 50);
        checkEq(planned.size(), size_t(1), "one chunk before fit");
        checkEq(prompts(fit(planned, 8, tok, kRate)),
                Strings{"One two, three four five six.", "Seven eight nine ten."}, "sentence end over comma");
    }
    {   // fit_keeps_the_words_in_order_and_every_piece_fits
        Words tok;
        std::string text;
        for (int i = 0; i < 60; ++i) {
            if (i) text += ' ';
            text += "w" + std::to_string(i) + (i % 7 == 6 ? "," : "");
        }
        const auto c = fitText(tok, text, 6);
        check(allFit(c, 6), "every piece fits 6");
        std::string joined;
        for (const Chunk& chunk : c) joined += (joined.empty() ? "" : " ") + chunk.text;
        checkEq(joined, text, "words kept in order");
    }
    {   // fit_leaves_chunks_that_fit_alone
        Words tok;
        const auto planned = plan(tok, "one two. three four.", 3);
        checkEq(texts(fit(planned, 3, tok, kRate)), texts(planned), "fitting chunks untouched");
    }
    {   // fit_cuts_a_word_that_is_too_long_on_its_own
        Chars tok;
        const auto c = fitText(tok, "abcdefghijkl", 5);
        checkEq(texts(c), Strings{"abc", "def", "ghi", "jkl"}, "word cut between characters");
        check(allFit(c, 5), "word pieces fit");
        std::string e9;
        for (int i = 0; i < 9; ++i) e9 += "é";
        std::string joined;
        for (const Chunk& chunk : fitText(tok, e9, 4)) joined += chunk.text;
        checkEq(joined, e9, "multi-byte word cut on character boundaries");
    }
    {   // fit_keeps_a_word_that_fits_the_models_larger_limit
        Chars tok;
        const auto c = fitWithWordLimit({makeChunk("hi abcdefghijkl bye", tok, kRate)}, 5, 13, tok, kRate);
        checkEq(texts(c), Strings{"hi", "abcdefghijkl", "bye"}, "word limit texts");
        checkEq(prompts(c), Strings{"Hi.", "Abcdefghijkl.", "Bye."}, "word limit prompts");
        const auto d = fitWithWordLimit({makeChunk("abcdefghijklmnop", tok, kRate)}, 5, 13, tok, kRate);
        checkEq(texts(d), Strings{"abcdefgh", "ijklmnop"}, "word over the word limit");
        check(allFit(d, 13), "word pieces fit the word limit");
    }
    {   // fit_or_error_accepts_a_long_word_between_sentences
        Chars tok;
        const std::string text = "Your number is " + std::string(40, '9') + ". Thank you.";
        const auto c = fitOrError(plan(tok, text, 50), 20, tok, kRate);
        check(allFit(c, 20), "long word pieces fit");
        auto noSpaces = [](std::string s) { s.erase(std::remove(s.begin(), s.end(), ' '), s.end()); return s; };
        std::string spoken;
        for (const Chunk& chunk : c) spoken += noSpaces(chunk.text);
        checkEq(spoken, noSpaces(text), "long word kept whole across pieces");
    }
    {   // fit_or_error_bounds_a_long_sentence_before_cache_sizing
        Words tok;
        std::string text;
        for (int i = 0; i < 225; ++i) text += i ? " word" : "word";
        const auto planned = plan(tok, text, 50);
        check(planned[0].tokens.size() > kMaxFitTokens, "planned chunk is over 200");
        check(allFit(fitOrError(planned, kMaxFitTokens, tok, kRate), kMaxFitTokens), "fitted to 200");
        bool threw = false;
        try { fitOrError(plan(tok, "word", 50), 1, tok, kRate); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "unsplittable piece is an error");
    }
    {   // max_tokens_for_is_the_largest_chunk_that_fits
        checkEq(maxTokensFor(0, kRate), size_t(0), "max tokens for 0");
        for (size_t room : {30, 100, 796, 3971}) {
            const size_t n = maxTokensFor(room, kRate);
            check(n + frameBudget(n, kRate) <= room, "max tokens fits room " + std::to_string(room));
            check(n + 1 + frameBudget(n + 1, kRate) > room, "max tokens is largest for " + std::to_string(room));
        }
    }
    {   // frame_budget_matches_the_frontends, frame_budget_follows_the_frame_rate
        for (size_t n : {0, 1, 7, 12, 50, 137})
            checkEq(frameBudget(n, kRate), static_cast<size_t>(std::ceil((n / 3.0 + 2.0) * 12.5)), "frame budget");
        checkEq(frameBudget(0, kRate), size_t(25), "frame budget 0");
        checkEq(frameBudget(50, kRate), size_t(234), "frame budget 50");
        checkEq(frameBudget(30, kRate), frameBudget(30, 25.0) / 2, "frame budget rate");
    }
}

class SpmTokenizer final : public Tokenizer {
    public:
    explicit SpmTokenizer(const std::string& path) {
        if (!_spm.Load(path).ok()) throw std::runtime_error("cannot load " + path);
    }
    std::vector<int> encode(std::string_view text) const override {
        std::vector<int> ids;
        if (!_spm.Encode(std::string(text), &ids).ok()) throw std::runtime_error("encode");
        return ids;
    }
    std::string decode(const std::vector<int>& tokens) const override {
        std::string out;
        if (!_spm.Decode(tokens, &out).ok()) throw std::runtime_error("decode");
        return out;
    }

    private:
    sentencepiece::SentencePieceProcessor _spm;
};

void testGolden(const Tokenizer& tok, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ++g_failures;
        std::cerr << "FAIL: cannot open " << path << "\n";
        return;
    }
    size_t cases = 0, mismatches = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto v = nlohmann::json::parse(line);
        const std::string kind = v["kind"], text = v["text"];
        nlohmann::json got;
        if (kind == "tokens") {
            got = tok.encode(text);
        } else if (kind == "chunks") {
            const std::string lang = v["lang"], rules = v["rules"];
            const size_t maxTokens = v["max_tokens"];
            const std::optional<size_t> fitTo =
                v["fit"].is_null() ? std::nullopt : std::optional<size_t>(v["fit"].get<size_t>());
            try {
                got = nlohmann::json::array();
                for (const Chunk& c : planChunks(tok, text, parseLang(lang), *Rules::parse(rules), kRate, maxTokens, fitTo))
                    got.push_back({{"text", c.text}, {"tokens", c.tokens}, {"frames_after_eos", c.framesAfterEos},
                                   {"frame_budget", c.frameBudget}});
            } catch (const std::invalid_argument& e) {
                got = {{"error", e.what()}};
            }
        } else {
            continue;
        }
        ++cases;
        const auto& want = v["expected"];
        // Errors match on kind, not wording: the reference prefixes its own error type.
        const bool same = want.is_object() ? got.is_object() : got == want;
        if (!same) {
            if (++mismatches <= 20)
                std::cerr << "MISMATCH " << kind << " " << line.substr(0, 300) << "\n   got: " << got.dump().substr(0, 600)
                          << "\n";
        }
    }
    std::cerr << path << ": " << cases - mismatches << "/" << cases << " match\n";
    g_failures += static_cast<int>(mismatches);
}

} // namespace

int main(int argc, char** argv) {
    testPlanRs();
    if (argc >= 2) {
        if (!std::filesystem::exists(argv[1])) {
            std::cerr << "skip: no tokenizer at " << argv[1] << " (download the Phonon checkpoint)\n";
            return g_failures ? 1 : 77;
        }
        SpmTokenizer tok(argv[1]);
        for (int i = 2; i < argc; ++i) testGolden(tok, argv[i]);
    } else {
        std::cerr << "note: no tokenizer given, only the plan.rs unit tests ran\n";
    }
    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "all checks passed\n";
    return 0;
}
