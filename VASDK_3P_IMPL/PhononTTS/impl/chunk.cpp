// chunk.cpp — port of gradium-ai/xn-ptts v0.4.1 ptts/src/plan.rs (chunks, fit, cut) and
// ptts/src/tts_model.rs (split_into_best_sentences). See chunk.h.

#include "chunk.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gradium::tts::text {

namespace {

std::string join(const std::vector<std::string_view>& words, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
        if (i > from) out += ' ';
        out += words[i];
    }
    return out;
}

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// `word.trim_end_matches(closers).ends_with(marks)`, with the closers of plan.rs `cut`:
// a mark can sit inside closing quotes or brackets.
bool endsWithMark(std::string_view word, std::string_view marks) {
    static constexpr std::string_view kClosers[] = {"\"", "'", ")", "]", "”", "’", "»"};
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (std::string_view c : kClosers) {
            if (word.size() >= c.size() && word.substr(word.size() - c.size()) == c) {
                word.remove_suffix(c.size());
                stripped = true;
                break;
            }
        }
    }
    return !word.empty() && marks.find(word.back()) != std::string_view::npos;
}

// plan.rs `cut`: where to cut `words`, as the number of words before the cut — after the
// sentence end nearest the middle in tokens that leaves `minSide` tokens on each side, else
// after such a clause mark, else after the word nearest the middle.
size_t cut(const std::vector<std::string_view>& words, size_t minSide, const Tokenizer& tok) {
    if (words.size() < 2) throw std::invalid_argument("nothing to cut: fewer than two words");
    // ends[i] is the number of tokens in words[..=i].
    std::vector<size_t> ends;
    ends.reserve(words.size());
    size_t total = 0;
    for (std::string_view w : words) {
        total += tok.encode(w).size();
        ends.push_back(total);
    }
    const auto imbalance = [&](size_t end) {
        const size_t rest = total - end;
        return end > rest ? end - rest : rest - end;
    };
    // min_by_key keeps the first of equal minima.
    const auto best = [&](auto&& accept) -> std::optional<size_t> {
        std::optional<size_t> found;
        for (size_t i = 0; i + 1 < words.size(); ++i) {
            if (!accept(i)) continue;
            if (!found || imbalance(ends[i]) < imbalance(ends[*found])) found = i;
        }
        return found;
    };
    const auto after = [&](std::string_view marks) {
        return best([&](size_t i) {
            return endsWithMark(words[i], marks) && ends[i] >= minSide && total - ends[i] >= minSide;
        });
    };
    std::optional<size_t> i = after(".!?");
    if (!i) i = after(",;:");
    if (!i) i = best([](size_t) { return true; });
    return *i + 1;
}

} // namespace

size_t frameBudget(size_t numTokens, double frameRate) {
    return static_cast<size_t>(std::ceil((static_cast<double>(numTokens) / 3.0 + 2.0) * frameRate));
}

size_t maxTokensFor(size_t room, double frameRate) {
    for (size_t n = room + 1; n-- > 0;)
        if (n + frameBudget(n, frameRate) <= room) return n;
    return 0;
}

Chunk makeChunk(std::string text, const Tokenizer& tok, double frameRate) {
    const Prompt prompt = prepareTextPrompt(text);
    Chunk c;
    c.tokens = tok.encode(prompt.text);
    c.text = std::move(text);
    c.framesAfterEos = prompt.framesAfterEos;
    c.frameBudget = frameBudget(c.tokens.size(), frameRate);
    return c;
}

// tts_model.rs: split_into_best_sentences.
std::vector<std::string> splitIntoBestSentences(const Tokenizer& tok, std::string_view text, size_t maxTokens) {
    const std::string prepared(detail::strTrim(prepareTextPrompt(text).text));
    const std::vector<int> tokens = tok.encode(prepared);

    // End-of-sentence token ids: ".!...?" tokenized, skipping the first token (it carries
    // SentencePiece's leading-space marker).
    const std::vector<int> marker = tok.encode(".!...?");
    const std::vector<int> eosTokens(marker.size() > 1 ? marker.begin() + 1 : marker.begin(), marker.end());

    // Sentence boundaries: where a non-EOS token follows one or more EOS tokens.
    std::vector<size_t> boundaries{0};
    bool prevWasEos = false;
    for (size_t idx = 0; idx < tokens.size(); ++idx) {
        if (std::find(eosTokens.begin(), eosTokens.end(), tokens[idx]) != eosTokens.end()) {
            prevWasEos = true;
        } else {
            if (prevWasEos) boundaries.push_back(idx);
            prevWasEos = false;
        }
    }
    boundaries.push_back(tokens.size());

    std::vector<std::pair<size_t, std::string>> sentences;
    for (size_t w = 0; w + 1 < boundaries.size(); ++w) {
        const size_t start = boundaries[w], end = boundaries[w + 1];
        sentences.emplace_back(end - start,
                               tok.decode(std::vector<int>(tokens.begin() + start, tokens.begin() + end)));
    }

    // Greedily group sentences into chunks that stay under maxTokens.
    std::vector<std::string> out;
    std::string current;
    size_t currentTokens = 0;
    for (auto& [n, sentence] : sentences) {
        if (current.empty()) {
            current = std::move(sentence);
            currentTokens = n;
            continue;
        }
        if (currentTokens + n > maxTokens) {
            out.emplace_back(detail::strTrim(current));
            current = std::move(sentence);
            currentTokens = n;
        } else {
            current += ' ';
            current += sentence;
            currentTokens += n;
        }
    }
    if (!current.empty()) out.emplace_back(detail::strTrim(current));
    return out;
}

// plan.rs: chunks.
std::vector<Chunk> chunks(const Tokenizer& tok, std::string_view text, std::optional<Lang> lang,
                          Rules rules, size_t maxTokens, double frameRate) {
    const std::string normalized = lang ? normalizeText(text, *lang, rules) : std::string(text);
    std::vector<Chunk> out;
    for (std::string& piece : splitIntoBestSentences(tok, normalized, maxTokens))
        out.push_back(makeChunk(std::move(piece), tok, frameRate));
    if (out.empty()) throw std::invalid_argument("nothing to synthesize: the text is empty");
    return out;
}

std::vector<Chunk> fit(std::vector<Chunk> chunks, size_t max, const Tokenizer& tok, double frameRate) {
    return fitWithWordLimit(std::move(chunks), max, max, tok, frameRate);
}

// plan.rs: fit_with_word_limit.
std::vector<Chunk> fitWithWordLimit(std::vector<Chunk> chunks, size_t max, size_t wordLimit,
                                    const Tokenizer& tok, double frameRate) {
    wordLimit = std::max(wordLimit, max);
    std::vector<Chunk> fitted;
    fitted.reserve(chunks.size());
    std::vector<Chunk> todo(std::make_move_iterator(chunks.rbegin()), std::make_move_iterator(chunks.rend()));
    while (!todo.empty()) {
        Chunk chunk = std::move(todo.back());
        todo.pop_back();
        if (chunk.tokens.size() <= max) {
            fitted.push_back(std::move(chunk));
            continue;
        }
        const std::vector<std::string_view> words = detail::splitWhitespace(chunk.text);
        std::string halves[2];
        if (words.size() >= 2) {
            const size_t k = cut(words, (max + 3) / 4, tok);
            halves[0] = join(words, 0, k);
            halves[1] = join(words, k, words.size());
        } else {
            if (chunk.tokens.size() <= wordLimit) {
                fitted.push_back(std::move(chunk));
                continue;
            }
            // Cut the word between its characters: at char index count / 2.
            const std::string_view word = detail::strTrim(chunk.text);
            size_t count = 0;
            for (char c : word) count += !isContinuation(c);
            size_t mid = 0;
            for (size_t seen = 0; mid < word.size(); ++mid) {
                if (isContinuation(word[mid])) continue;
                if (seen++ == count / 2) break;
            }
            if (mid == 0 || mid >= word.size()) {
                fitted.push_back(std::move(chunk));
                continue;
            }
            halves[0] = std::string(word.substr(0, mid));
            halves[1] = std::string(word.substr(mid));
        }
        todo.push_back(makeChunk(std::move(halves[1]), tok, frameRate));
        todo.push_back(makeChunk(std::move(halves[0]), tok, frameRate));
    }
    return fitted;
}

std::vector<Chunk> fitOrError(std::vector<Chunk> chunks, size_t max, const Tokenizer& tok, double frameRate) {
    std::vector<Chunk> fitted = fit(std::move(chunks), max, tok, frameRate);
    for (const Chunk& c : fitted)
        if (c.tokens.size() > max)
            throw std::invalid_argument("a text chunk is " + std::to_string(c.tokens.size()) +
                                        " tokens and cannot fit the " + std::to_string(max) +
                                        "-token limit; shorten or split it");
    return fitted;
}

// synth.rs: plan_chunks, as Synth::stream_with_rng calls it.
std::vector<Chunk> planChunks(const Tokenizer& tok, std::string_view text, std::optional<Lang> lang,
                              Rules rules, double frameRate, size_t maxTokens, std::optional<size_t> fitTo) {
    std::vector<Chunk> planned = chunks(tok, text, lang, rules, maxTokens, frameRate);
    return fitTo ? fitOrError(std::move(planned), *fitTo, tok, frameRate) : planned;
}

} // namespace gradium::tts::text
