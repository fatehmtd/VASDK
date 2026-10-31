// Tests for the MOSS-TTS-Nano text normalizer.
//
//  1. Parity of robustNormalize() with the reference normalize_tts_text, on the reference's
//     own test table (tests/golden/robust_golden.jsonl, see tests/golden-gen), including its
//     idempotence check.
//  2. Idempotence on assistant-style inputs outside that table.
//  3. Expected spoken forms of the semantic stage in English, Chinese and Japanese.
//
// Usage: moss_normalize_test <robust_golden.jsonl>

#include "MossTextNormalizer.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using moss::text::Lang;

namespace {

struct Checker {
    int failures = 0;
    int checks   = 0;

    void expectEq(const std::string& what, const std::string& input, const std::string& actual,
                  const std::string& expected) {
        ++checks;
        if (actual == expected) return;
        ++failures;
        std::printf("FAIL [%s]\n  input   : %s\n  expected: %s\n  actual  : %s\n\n",
                    what.c_str(), input.c_str(), expected.c_str(), actual.c_str());
    }
};

void robustParity(Checker& chk, const char* goldenPath) {
    std::ifstream f(goldenPath, std::ios::binary);
    if (!f) {
        std::printf("FAIL cannot open %s\n", goldenPath);
        ++chk.failures;
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto j = nlohmann::json::parse(line);
        const std::string name = j.at("name"), input = j.at("input"), expected = j.at("expected");
        chk.expectEq(name, input, moss::text::robustNormalize(input), expected);
        chk.expectEq(name + "_idempotence", expected, moss::text::robustNormalize(expected), expected);
    }
}

void robustIdempotence(Checker& chk) {
    const char* const inputs[] = {
        "Sure! Here's what I found:\n\n1. **Weather**: sunny\n2. Temperature -- 24°C...",
        "Visit https://example.com/path_to?x=1 for more!!!",
        "Hello   world...   How are you???",
        "It costs $5.50 (approx.) -> cheap",
        "Call me at 555-123-4567 or email john_doe@example.com",
        "The model [beta] is {fast} — really",
        "Version v1.2.3 of app_config.yaml",
        "中文English混合text测试 123 个",
        "日本語のテキスト、テスト。カタカナ ABC",
        "line one\r\nline two\r\n\r\n- bullet\n> quote\n## Title",
        "Wait…what?! Really?!?!",
        "@user_name check #topic and r/cpp",
        "a<->b <= c => d",
        "See [the docs](http://docs.example.org/a_b) now",
        "【公告】 明天 10:00 开会 —— 请准时",
    };
    for (const char* in : inputs) {
        const std::string once = moss::text::robustNormalize(in);
        chk.expectEq("idempotence", in, moss::text::robustNormalize(once), once);
    }
    // A few fixed expectations outside the reference table. "world..." is kept: the
    // reference's file-name rule protects a letter run containing '.'.
    chk.expectEq("robust_en", "Hello   world...   How are you???",
             moss::text::robustNormalize("Hello   world...   How are you???"), "Hello world... How are you？");
    chk.expectEq("robust_ellipsis", "Wait... what",
             moss::text::robustNormalize("Wait ... what"), "Wait。what。");
    chk.expectEq("robust_empty", "", moss::text::robustNormalize("   "), "");
}

struct Case { Lang lang; const char* in; const char* out; };

void expansion(Checker& chk) {
    const Case cases[] = {
        // --- English ---
        {Lang::En, "I have 3 apples", "I have three apples"},
        {Lang::En, "It costs $5.50", "It costs five dollars fifty cents"},
        {Lang::En, "only $1", "only one dollar"},
        {Lang::En, "$0.99 each", "ninety nine cents each"},
        {Lang::En, "a $2.5 billion deal", "a two point five billion dollars deal"},
        {Lang::En, "50% off", "fifty percent off"},
        {Lang::En, "1,234,567", "one million two hundred thirty four thousand five hundred sixty seven"},
        {Lang::En, "pi is 3.14", "pi is three point one four"},
        {Lang::En, "it is -2 outside", "it is negative two outside"},
        {Lang::En, "the 21st century", "the twenty first century"},
        {Lang::En, "the 3rd time", "the third time"},
        {Lang::En, "in 1999", "in nineteen ninety nine"},
        {Lang::En, "in 2024", "in twenty twenty four"},
        {Lang::En, "since 2005", "since two thousand five"},
        {Lang::En, "May 2024", "May twenty twenty four"},
        {Lang::En, "2024 apples", "two thousand twenty four apples"},
        {Lang::En, "the 1990s", "the nineteen nineties"},
        {Lang::En, "at 10:30", "at ten thirty"},
        {Lang::En, "at 9:05 am", "at nine oh five a m"},
        {Lang::En, "at 3pm", "at three p m"},
        {Lang::En, "at 7 p.m. sharp", "at seven p m sharp"},
        {Lang::En, "10:00", "ten o'clock"},
        {Lang::En, "2024-05-01", "May first, twenty twenty four"},
        {Lang::En, "12/25/2023", "December twenty fifth, twenty twenty three"},
        {Lang::En, "1/2 cup", "one half cup"},
        {Lang::En, "3/4 done", "three quarters done"},
        {Lang::En, "open 24/7", "open twenty four seven"},
        {Lang::En, "5km away", "five kilometers away"},
        {Lang::En, "1 kg", "one kilogram"},
        {Lang::En, "25°C", "twenty five degrees Celsius"},
        {Lang::En, "60 mph", "sixty miles per hour"},
        {Lang::En, "5 m tall", "five m tall"},
        {Lang::En, "call 555-123-4567", "call five five five, one two three, four five six seven"},
        {Lang::En, "pages 10-20", "pages ten to twenty"},
        {Lang::En, "from 1990-2000", "from nineteen ninety to two thousand"},
        {Lang::En, "v2.3.1", "v two point three point one"},
        {Lang::En, "GPT4", "GPT four"},
        {Lang::En, "5k users", "five thousand users"},
        {Lang::En, "code 007", "code zero zero seven"},
        {Lang::En, "id 1234567890", "id one two three four five six seven eight nine zero"},
        {Lang::En, "No. 5", "number five"},
        {Lang::En, "#1 fan", "number one fan"},
        {Lang::En, "2+2=4", "two plus two equals four"},
        {Lang::En, "Dr. Smith vs. Mr. Jones", "Doctor Smith versus Mister Jones"},
        {Lang::En, "e.g. apples", "for example apples"},
        {Lang::En, "apples, pears, etc.", "apples, pears, et cetera."},
        {Lang::En, "visit https://github.com/OpenMOSS", "visit github dot com slash OpenMOSS"},
        {Lang::En, "mail john_doe@example.com", "mail john doe at example dot com"},
        {Lang::En, "open package.json now", "open package dot json now"},
        {Lang::En, "edit .env first", "edit dot env first"},
        {Lang::En, "COVID-19", "COVID-nineteen"},
        {Lang::En, "Japan.Next", "Japan.Next"},
        // --- Chinese ---
        {Lang::Zh, "3月31日", "三月三十一日"},
        {Lang::Zh, "2026年", "二零二六年"},
        {Lang::Zh, "2024-05-01", "二零二四年五月一日"},
        {Lang::Zh, "今天20:00维护", "今天二十点维护"},
        {Lang::Zh, "9:05", "九点零五分"},
        {Lang::Zh, "50%", "百分之五十"},
        {Lang::Zh, "3.5%", "百分之三点五"},
        {Lang::Zh, "123", "一百二十三"},
        {Lang::Zh, "10", "十"},
        {Lang::Zh, "15个", "十五个"},
        {Lang::Zh, "110", "一百一十"},
        {Lang::Zh, "1005", "一千零五"},
        {Lang::Zh, "10001", "一万零一"},
        {Lang::Zh, "100000", "十万"},
        {Lang::Zh, "100000000", "一亿"},
        {Lang::Zh, "为-2", "为负二"},
        {Lang::Zh, "¥100", "一百元"},
        {Lang::Zh, "$5", "五美元"},
        {Lang::Zh, "25℃", "二十五摄氏度"},
        {Lang::Zh, "1/2", "二分之一"},
        {Lang::Zh, "10-20", "十到二十"},
        {Lang::Zh, "13812345678", "一三八一二三四五六七八"},
        {Lang::Zh, "5km", "五千米"},
        {Lang::Zh, "3.14", "三点一四"},
        {Lang::Zh, "第3名", "第三名"},
        {Lang::Zh, "１２３", "一百二十三"},
        // --- Japanese ---
        {Lang::Ja, "2024年", "二千二十四年"},
        {Lang::Ja, "100円", "百円"},
        {Lang::Ja, "1500", "千五百"},
        {Lang::Ja, "20:30", "二十時三十分"},
        {Lang::Ja, "50%", "五十パーセント"},
        {Lang::Ja, "10000", "一万"},
        {Lang::Ja, "-3", "マイナス三"},
        {Lang::Ja, "1005", "千五"},
    };
    const char* const names[] = {"en", "zh", "ja"};
    for (const Case& c : cases)
        chk.expectEq(std::string("expand_") + names[static_cast<int>(c.lang)], c.in,
                 moss::text::expand(c.in, c.lang), c.out);
}

void pipeline(Checker& chk) {
    chk.expectEq("detect_ja", "", moss::text::detectLanguage("今日はいい天気", false) == Lang::Ja ? "ja" : "x", "ja");
    chk.expectEq("detect_zh", "", moss::text::detectLanguage("今天天气很好", true) == Lang::Zh ? "zh" : "x", "zh");
    chk.expectEq("detect_en", "", moss::text::detectLanguage("Hello", false) == Lang::En ? "en" : "x", "en");
    chk.expectEq("detect_voice", "", moss::text::detectLanguage("123", true) == Lang::En ? "en" : "x", "en");

    const struct { bool english; const char* in; const char* out; } cases[] = {
        {true,  "**Sure!** It's 25°C today 🌞", "Sure! It's twenty five degrees Celsius today。"},
        {true,  "- Buy 2 apples\n- Pay $3", "Buy two apples。Pay three dollars。"},
        {false, "价格是¥100，折扣50%", "价格是一百元，折扣百分之五十。"},
        {false, "请求接入-身份判定", "请求接入，身份判定。"},
        {false, "会议在2024-05-01 9:30开始", "会议在二零二四年五月一日九点三十分开始。"},
        {false, "明日は15時に会いましょう", "明日は十五時に会いましょう。"},
    };
    for (const auto& c : cases)
        chk.expectEq("normalize", c.in, moss::text::normalize(c.in, c.english), c.out);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <robust_golden.jsonl>\n", argv[0]);
        return 2;
    }
    Checker chk;
    robustParity(chk, argv[1]);
    robustIdempotence(chk);
    expansion(chk);
    pipeline(chk);
    std::printf("%d/%d checks passed\n", chk.checks - chk.failures, chk.checks);
    return chk.failures == 0 ? 0 : 1;
}
