#pragma once
// normalize.h — text normalization applied before tokenization: a C++ port of
// gradium-ai/xn-ptts v0.4.1 `ptts::preprocess` (preprocess.rs and preprocess/rewrite/*.rs).
//
// The port follows the reference rule for rule, quirks included ("$1" reads "1 dollars",
// "1,23" reads "123"), so that the model hears exactly what Gradium's runtime would give it.
// Lengths inside the rewrite rules are UTF-8 byte lengths, as in the Rust code, and the
// Unicode properties come from tables generated from Rust's core::char (unicode_props.inc).
// tests/normalize_test.cpp checks the port against the reference: every unit test of the
// Rust source, and golden files the reference produced (tests/golden/).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gradium::tts::text {

    // Language driving the spoken forms. There is deliberately no default: normalizing German
    // as English speaks "@" as "at" rather than "ät", so guessing is worse than not normalizing.
    enum class Lang { En, Fr, De, Es, Pt };

    // en, fr, de, es or pt, any case, surrounding whitespace ignored.
    std::optional<Lang> parseLang(std::string_view s);
    const char* langCode(Lang lang);

    // Which of the word rewrite rules run, on top of the character normalization. Bit i is
    // rule i of: numbers, currency, dashed-digits, emails, urls, abbreviations, elongations,
    // phones, times, dates.
    struct Rules {
        uint32_t bits = 0;

        static Rules none();
        // Every rule but phones, times and dates, as the reference runs by default.
        static Rules defaults();
        static Rules all();

        // "default", "all", "none" / "off", or a comma-separated list of rule names, any case.
        static std::optional<Rules> parse(std::string_view s);

        bool has(std::string_view name) const;
        bool operator==(const Rules& o) const { return bits == o.bits; }
    };

    // Rewrite UTF-8 `input` into the character set the model was trained on, spelling numbers,
    // amounts, addresses, symbols and abbreviations out in `lang` as `rules` allow.
    std::string normalizeText(std::string_view input, Lang lang, Rules rules);

    // The prompt the model is given for one piece of (normalized) text, as
    // `ptts::tts_model::prepare_text_prompt` builds it: whitespace collapsed, the first letter
    // capitalized, a full stop after a final letter or digit, and how many frames to keep
    // generating once the model signals the end of speech (3 for up to four words, else 1).
    struct Prompt {
        std::string text;
        int framesAfterEos = 3;
    };
    Prompt prepareTextPrompt(std::string_view text);

    // The individual rules and helpers, exposed for the parity tests.
    namespace detail {
        std::optional<std::string> rewriteWord(std::string_view word, Lang lang, Rules rules);

        std::optional<std::string> numbers(std::string_view word, Lang lang);
        std::optional<std::string> currency(std::string_view word, Lang lang);
        std::optional<std::string> dashedDigits(std::string_view word, Lang lang);
        std::optional<std::string> emails(std::string_view word, Lang lang);
        std::optional<std::string> urls(std::string_view word, Lang lang);
        std::optional<std::string> abbreviations(std::string_view word, Lang lang);
        std::optional<std::string> elongations(std::string_view word, Lang lang);
        std::optional<std::string> phones(std::string_view word, Lang lang);
        std::optional<std::string> times(std::string_view word, Lang lang);
        std::optional<std::string> dates(std::string_view word, Lang lang);

        // Multi-word spans: (rewritten text, words used).
        std::optional<std::pair<std::string, size_t>> ranks(const std::vector<std::string_view>& words, Lang lang);
        std::optional<std::pair<std::string, size_t>> scaled(const std::vector<std::string_view>& words, Lang lang);
        std::optional<std::pair<std::string, size_t>> digitRun(const std::vector<std::string_view>& words, Lang lang);
        std::optional<std::pair<std::string, size_t>> endingIn(std::optional<std::string_view> prev,
                                                               const std::vector<std::string_view>& words, Lang lang);

        bool isTld(std::string_view label);
        std::pair<std::string_view, std::string_view> splitSuffix(std::string_view word);
        std::optional<std::pair<std::string_view, std::string_view>> digits(std::string_view s, size_t min, size_t max);
        std::string spellShort(std::string_view word);

        // Rust's str::trim and str::split_whitespace (Unicode White_Space), for the chunker.
        std::string_view strTrim(std::string_view s);
        std::vector<std::string_view> splitWhitespace(std::string_view s);
    } // namespace detail

} // namespace gradium::tts::text
