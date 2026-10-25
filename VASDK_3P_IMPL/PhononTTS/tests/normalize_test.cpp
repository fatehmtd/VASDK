// Parity tests for the C++ port of the Phonon text normalizer (impl/normalize.cpp) against the
// reference, gradium-ai/xn-ptts v0.4.1 `ptts::preprocess`.
//
//   phonon_normalize_test GOLDEN.jsonl
//
// GOLDEN.jsonl was produced by the reference itself (tests/golden-gen): every string of every
// unit test of the reference's normalizer sources, in every language, plus realistic sentences
// and a seeded fuzz corpus, each with the reference's output. A line is
//   {"kind": "text", "lang", "rules", "text", "expected"}  normalize_text(text, lang, rules)
//   {"kind": "word", "lang", "rules", "text", "expected"}  rewrite_word(text, lang, rules), or null
//   {"kind": "prompt", ..., "text", "expected": [s, n]}    tts_model::prepare_text_prompt(text)
// The port must reproduce every expected output byte for byte.
//
// On top of that, the checks below port the reference's tests of what the golden file cannot
// reach: the rule-set and language parsers, and the helpers of rewrite.rs.

#include "normalize.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
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

Rules rules(const char* s) { return *Rules::parse(s); }

// rewrite.rs: rules_pick_what_runs, numbers_win_over_times, rules_parse_lists_spaces_and_case.
void testRules() {
    checkEq(Rules::parse("all"), std::optional<Rules>(Rules::all()), "all");
    checkEq(Rules::parse("default"), std::optional<Rules>(Rules::defaults()), "default");
    const Rules def = rules("numbers,currency,dashed-digits,emails,urls,abbreviations,elongations");
    checkEq(def, Rules::defaults(), "default list");
    const Rules optIn = rules("phones,times,dates");
    checkEq(Rules{def.bits | optIn.bits}, Rules::all(), "default + opt-in = all");
    checkEq(Rules::parse("none"), std::optional<Rules>(Rules::none()), "none");
    check(!Rules::parse("colours"), "unknown rule");
    checkEq(detail::rewriteWord("1234", Lang::En, Rules::all()), std::optional<std::string>("1 thousand 234"), "1234 en");
    checkEq(detail::rewriteWord("1234", Lang::Fr, Rules::all()), std::optional<std::string>("mille 234"), "1234 fr");
    checkEq(detail::rewriteWord("1234", Lang::En, Rules::none()), std::optional<std::string>(), "1234 none");
    checkEq(detail::rewriteWord("1234", Lang::De, Rules::all()), std::optional<std::string>("ein Tausend 234"), "1234 de");
    checkEq(detail::rewriteWord("$5", Lang::En, Rules::defaults()), std::optional<std::string>("5 dollars"), "$5");
    checkEq(detail::rewriteWord("3:45PM", Lang::En, Rules::defaults()), std::optional<std::string>(), "3:45PM default");
    checkEq(detail::rewriteWord("3:45PM", Lang::En, Rules::all()), std::optional<std::string>("3-45 PM"), "3:45PM all");
    checkEq(detail::rewriteWord("1.234", Lang::De, Rules::all()), std::optional<std::string>("ein Tausend 234"), "1.234 de");
    checkEq(detail::rewriteWord("22.45", Lang::De, Rules::all()), std::optional<std::string>("22 Uhr 45"), "22.45 de");

    const Rules numbers = rules("numbers");
    checkEq(Rules::parse(" Numbers "), std::optional<Rules>(numbers), "' Numbers '");
    checkEq(Rules::parse("numbers, numbers"), std::optional<Rules>(numbers), "numbers, numbers");
    checkEq(Rules::parse(" Emails , URLS"), Rules::parse("emails,urls"), "' Emails , URLS'");
    checkEq(Rules::parse("ALL"), std::optional<Rules>(Rules::all()), "ALL");
    checkEq(Rules::parse(" Default "), std::optional<Rules>(Rules::defaults()), "' Default '");
    checkEq(Rules::parse("Off"), std::optional<Rules>(Rules::none()), "Off");
    check(!Rules::parse("numbers,colours"), "numbers,colours");
    check(!Rules::parse(""), "empty");
    check(!Rules::parse("numbers,all"), "numbers,all");
}

// preprocess.rs: lang_round_trips_through_str.
void testLang() {
    checkEq(parseLang("en"), std::optional<Lang>(Lang::En), "en");
    checkEq(parseLang("FR"), std::optional<Lang>(Lang::Fr), "FR");
    checkEq(parseLang("de"), std::optional<Lang>(Lang::De), "de");
    checkEq(parseLang("es"), std::optional<Lang>(Lang::Es), "es");
    checkEq(parseLang("pt"), std::optional<Lang>(Lang::Pt), "pt");
    checkEq(parseLang(" en "), std::optional<Lang>(Lang::En), "' en '");
    check(!parseLang("klingon"), "klingon");
    for (Lang l : {Lang::En, Lang::Fr, Lang::De, Lang::Es, Lang::Pt})
        checkEq(parseLang(langCode(l)), std::optional<Lang>(l), std::string("round trip ") + langCode(l));
}

// rewrite.rs: helpers; tlds.rs: the_table_is_sorted_and_searchable.
void testHelpers() {
    using SV = std::pair<std::string_view, std::string_view>;
    checkEq(detail::splitSuffix("12,..."), SV("12", ",..."), "split_suffix 12,...");
    checkEq(detail::splitSuffix("12"), SV("12", ""), "split_suffix 12");
    checkEq(detail::splitSuffix("..."), SV("", "..."), "split_suffix ...");
    checkEq(detail::splitSuffix("a\xE2\x80\xA6"), SV("a", "\xE2\x80\xA6"), "split_suffix ellipsis");
    checkEq(detail::digits("12345x", 2, 4), std::optional<SV>(SV("1234", "5x")), "digits");
    checkEq(detail::digits("1x", 2, 4), std::optional<SV>(), "digits too short");
    checkEq(detail::spellShort("fr"), std::string("F-R"), "spell_short fr");
    checkEq(detail::spellShort("www"), std::string("W-W-W"), "spell_short www");
    checkEq(detail::spellShort("com"), std::string("com"), "spell_short com");
    checkEq(detail::spellShort("\xC3\xA9"), std::string("\xC3\xA9"), "spell_short é");
    check(detail::isTld("com") && detail::isTld("COM") && detail::isTld("Fr"), "tld case");
    check(detail::isTld("au") && detail::isTld("gmail") && detail::isTld("ie") && detail::isTld("pl"), "tld stray comma");
    check(!detail::isTld("html") && !detail::isTld("") && !detail::isTld("234"), "not tlds");
    // to_uppercase maps these to ASCII, so they can spell a top-level domain.
    check(detail::isTld("\xEF\xAC\x81"), "tld ligature fi"); // "ﬁ" -> "FI"
    check(detail::isTld("\xC3\x9F"), "tld sharp s");          // "ß" -> "SS"
}

std::optional<Lang> langOf(const std::string& s) { return parseLang(s); }

void testGolden(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "FAIL: cannot open golden file " << path << "\n";
        ++g_failures;
        return;
    }
    size_t total = 0, failed = 0, line = 0;
    std::string raw;
    while (std::getline(in, raw)) {
        ++line;
        if (raw.empty()) continue;
        const auto j = nlohmann::json::parse(raw);
        const std::string kind = j.at("kind"), langS = j.at("lang"), rulesS = j.at("rules"), text = j.at("text");
        const auto lang = langOf(langS);
        const auto rs = Rules::parse(rulesS);
        if (!lang || !rs) {
            std::cerr << "FAIL: line " << line << ": bad lang or rules\n";
            ++failed;
            continue;
        }
        ++total;
        std::optional<std::string> got, want;
        if (kind == "prompt") {
            // [prepared text, frames after EOS], compared as one string.
            const auto& e = j.at("expected");
            want = e.at(0).get<std::string>() + " | " + std::to_string(e.at(1).get<int>());
            const Prompt p = prepareTextPrompt(text);
            got = p.text + " | " + std::to_string(p.framesAfterEos);
        } else {
            if (!j.at("expected").is_null()) want = j.at("expected").get<std::string>();
            if (kind == "text") got = normalizeText(text, *lang, *rs);
            else got = detail::rewriteWord(text, *lang, *rs);
        }
        if (got != want) {
            if (++failed <= 40) {
                std::cerr << "MISMATCH line " << line << " [" << kind << " " << langS << " " << rulesS << "]\n"
                          << "  input:    " << nlohmann::json(text).dump() << "\n"
                          << "  expected: " << (want ? nlohmann::json(*want).dump() : "null") << "\n"
                          << "  got:      " << (got ? nlohmann::json(*got).dump() : "null") << "\n";
            }
        }
    }
    std::cerr << "golden: " << (total - failed) << "/" << total << " cases match the reference\n";
    g_failures += int(failed);
}

} // namespace

int main(int argc, char** argv) {
    testRules();
    testLang();
    testHelpers();
    for (int i = 1; i < argc; ++i) testGolden(argv[i]);
    if (argc < 2) std::cerr << "note: no golden file given, only the direct checks ran\n";
    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "all checks passed\n";
    return 0;
}
