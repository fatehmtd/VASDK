// StreamingReplyExtractor: every case is fed both as one delta and one character at a time,
// and must yield the same sentences, tail and shape either way.

#include <VASDK/llm/StreamingReplyExtractor.h>

#include <cstdio>
#include <string>
#include <vector>

using va::llm::StreamingReplyExtractor;
using Shape = StreamingReplyExtractor::Shape;

namespace {

struct Case {
    const char*              name;
    std::string              input;
    std::vector<std::string> sentences;
    std::string              tail;
    Shape                    shape;
};

int failures = 0;

void run(const Case& c, bool charByChar) {
    StreamingReplyExtractor ex("<think>", "</think>");
    std::vector<std::string> got;
    if (charByChar) {
        for (char ch : c.input)
            for (auto& s : ex.feed(std::string(1, ch))) got.push_back(s);
    } else {
        got = ex.feed(c.input);
    }
    const std::string tail = ex.flush();

    std::string expectedSpoken;
    for (auto& s : c.sentences) expectedSpoken += s;
    expectedSpoken += c.tail;

    if (got != c.sentences || tail != c.tail || ex.shape() != c.shape || ex.spokenText() != expectedSpoken) {
        ++failures;
        std::fprintf(stderr, "FAIL %s (%s): shape=%d tail='%s'\n", c.name, charByChar ? "char" : "whole",
                     static_cast<int>(ex.shape()), tail.c_str());
        for (auto& s : got) std::fprintf(stderr, "  sentence: '%s'\n", s.c_str());
    }
}

} // namespace

int main() {
    const std::vector<Case> cases = {
        {"text", R"({"text": "Sure, I can help. Your order shipped yesterday! Anything else?", "isExpecting": true})",
         {"Sure, I can help. ", "Your order shipped yesterday! "}, "Anything else?", Shape::Text},
        {"escapes", R"({"text":"He said \"hi\". Café time 😀 now. Done"})",
         {"He said \"hi\". ", "Caf\xC3\xA9 time \xF0\x9F\x98\x80 now. "}, "Done", Shape::Text},
        {"tools", R"({"tools":[{"tool":"lookup_order","args":{}}]})", {}, "", Shape::Tools},
        {"prose", "Hello there, friend. How are you?", {"Hello there, friend. "}, "How are you?", Shape::Prose},
        {"reasoning+fence", "<think>Plan it. Then answer. </think>\n```json\n{\"text\": \"Okay then. Bye now.\"}\n```",
         {"Okay then. "}, "Bye now.", Shape::Text},
        {"other json", R"({"isExpecting": true, "text": "Hi there. Bye."})", {}, "", Shape::OtherJson},
        {"prose then json", R"(Sure thing, friend. {"text": "x"})", {"Sure thing, friend. "}, "", Shape::Prose},
        {"short first sentence merges", R"({"text": "Hi. Okay, let me check."})", {}, "Hi. Okay, let me check.",
         Shape::Text},
    };
    for (const auto& c : cases) {
        run(c, false);
        run(c, true);
    }
    std::printf("%zu cases x 2 feeding modes, %d failure(s)\n", cases.size(), failures);
    return failures == 0 ? 0 : 1;
}
