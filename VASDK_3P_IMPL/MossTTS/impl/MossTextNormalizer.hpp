#ifndef MOSS_TEXT_NORMALIZER_HPP
#define MOSS_TEXT_NORMALIZER_HPP

// Text normalization for MOSS-TTS-Nano, following upstream's prepare_tts_request_texts
// (OpenMOSS/MOSS-TTS-Nano text_normalization_pipeline.py):
//
//   robust cleanup  →  [zh: hyphen guard]  →  semantic expansion  →  robust cleanup
//
// robustNormalize() is a rule-for-rule port of tts_robust_normalizer_single_script.py
// (normalize_tts_text): markdown / line handling, protection of URLs, emails, mentions,
// file names and versions, bracket and dash rewriting, punctuation and spacing cleanup.
// tests/normalize_test.cpp checks it against golden output of the Python reference.
//
// Upstream's semantic stage is WeTextProcessing (FST grammars for zh and en). That cannot be
// ported line for line, so expand() is a compact rewrite covering what assistant replies
// contain: cardinals, decimals, negatives, ordinals, percentages, money, dates, times,
// fractions, ranges, units, phone-like digit runs, versions, URLs / emails and common
// abbreviations, read in English, Chinese or Japanese.

#include <string>

namespace moss::text {

enum class Lang { En, Zh, Ja };

/// Language the semantic stage reads text in: Japanese when kana is present, Chinese when
/// CJK ideographs are, English when Latin letters are, else by voice
/// (resolve_text_normalization_language, plus a Japanese branch upstream lacks).
Lang detectLanguage(const std::string& text, bool englishVoice);

/// normalize_tts_text: non-semantic robustness cleanup.
std::string robustNormalize(const std::string& text);

/// _rewrite_hyphens_before_zh_wetext: keep numeric / negative hyphens, turn the rest
/// into pauses or spaces so the Chinese reader does not say "minus".
std::string rewriteHyphensForZh(const std::string& text);

/// Spell numbers, amounts, dates, symbols and abbreviations out in `lang`.
std::string expand(const std::string& text, Lang lang);

/// Full pipeline for one request.
std::string normalize(const std::string& text, bool englishVoice);

namespace detail {
std::u32string toU32(const std::string& s);
std::string    toUtf8(const std::u32string& s);
} // namespace detail

} // namespace moss::text

#endif // MOSS_TEXT_NORMALIZER_HPP
