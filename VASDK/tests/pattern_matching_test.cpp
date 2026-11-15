// Patterns as the rules understand them: what an agent's examples spell is matched like any
// example. The longest sentence found wins between intents, so "turn on [the] light" beats "turn
// on" for "turn on the light"; negation and negative examples still rule an intent out; the
// built-in intents' phrases in a language pack may be patterns too. Wildcards: `_` exactly one
// word, `*` any number, `+` one or more, at the end too; they count no words, so a specific
// example wins; `_` inside "@fan_level" is the name's; what reads words gets them without
// wildcards; lint stops an example of only wildcards and warns of a `*` at an end.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/ExamplePattern.h>
#include <VASDK/nlu/RuleNluProvider.h>

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace va;

namespace {

class PatternMatchingTest : public testing::Test {
protected:
    void compile(const std::string& text) {
        vasdk::flow::Agent agent;
        ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(text, &agent)) << text;
        std::string error;
        _agent = flow::CompiledAgent::compile(agent, &error);
        ASSERT_NE(_agent, nullptr) << error;
    }
    // The intent the rules understand in `words`, among the page's (as a conversation asks); "" for none.
    std::string understood(const std::string& words) const {
        nlu::NluRequest request;
        request.text = words;
        for (const auto& route : _agent->definition().flows(0).pages(0).routes()) request.candidates.push_back(route.intent());
        const auto result = _agent->rules()->understand(request);
        return result.intents.empty() ? std::string() : result.intents.front().intent;
    }

    std::shared_ptr<const flow::CompiledAgent> _agent;
};

constexpr const char* kLights = R"agent(
start_flow: "main"
phrase_sets { name: "light" phrases: ["flashlight", "torch", "(light|lights)"] }
intents { name: "light_on"  examples: ["(turn|switch) on [the] ${light}", "(turn|switch) [the] ${light} on", "${light} on"] }
intents { name: "light_off" examples: ["(turn|switch) off [the] ${light}", "(turn|switch) [the] ${light} off", "${light} off"]
          negative_examples: ["(don't|do not) (turn|switch) off"] }
intents { name: "turn_on"   examples: ["turn on"] }
flows { name: "main" start_page: "idle" pages { name: "idle"
  routes { intent: "light_on" } routes { intent: "light_off" } routes { intent: "turn_on" } } }
)agent";

TEST_F(PatternMatchingTest, EverySpelledSentenceIsUnderstood) {
    compile(kLights);
    EXPECT_EQ(understood("turn on the lights"), "light_on");
    EXPECT_EQ(understood("switch the torch on please"), "light_on");
    EXPECT_EQ(understood("flashlight on"), "light_on");
    EXPECT_EQ(understood("could you switch off the flashlight"), "light_off");
    EXPECT_EQ(understood("lights off"), "light_off");
}

TEST_F(PatternMatchingTest, TheLongestSentenceWins) {
    compile(kLights);
    EXPECT_EQ(understood("turn on the light"), "light_on") << "4 words beat \"turn on\"'s 2";
    EXPECT_EQ(understood("turn on the radio"), "turn_on") << "only \"turn on\" is in the words";
}

TEST_F(PatternMatchingTest, NegationStillRulesAnIntentOut) {
    compile(kLights);
    EXPECT_EQ(understood("don't turn on the light"), "") << "a negation just before";
    EXPECT_NE(understood("do not switch off the torch"), "light_off") << "a negative example, spelled by a pattern";
}

TEST_F(PatternMatchingTest, ALanguagePacksBuiltInPhrasesMayBePatterns) {
    compile(R"agent(
start_flow: "main"
language_defs { code: "en-US" name: "English"
                intents { key: "sys.cancel" value { phrases: ["(cancel|forget) [it|that]", "never mind"] } } }
flows { name: "main" start_page: "idle" pages { name: "idle" routes { intent: "sys.cancel" } } }
)agent");
    EXPECT_EQ(understood("forget that"), "sys.cancel");
    EXPECT_EQ(understood("cancel"), "sys.cancel");
    EXPECT_EQ(understood("never mind"), "sys.cancel");
}

constexpr const char* kWildcards = R"agent(
start_flow: "main"
intents { name: "light_on"  examples: ["turn _ (light|lights) on"] }
intents { name: "turn_on"   examples: ["turn * on"] }
intents { name: "call_back" examples: ["call + back"] }
intents { name: "remind"    examples: ["remind me to _"] }
intents { name: "specific"  examples: ["turn the radio on"] }
flows { name: "main" start_page: "idle" pages { name: "idle"
  routes { intent: "light_on" } routes { intent: "turn_on" } routes { intent: "call_back" } routes { intent: "remind" }
  routes { intent: "specific" } } }
)agent";

TEST_F(PatternMatchingTest, OneWordWildcardIsExactlyOneWord) {
    compile(kWildcards);
    EXPECT_EQ(understood("turn my light on"), "light_on");
    EXPECT_EQ(understood("please turn the lights on now"), "light_on");
    EXPECT_NE(understood("turn light on"), "light_on") << "no word where _ is";
    EXPECT_NE(understood("turn my bedroom light on"), "light_on") << "two words where _ is";
}

TEST_F(PatternMatchingTest, AnyWordsWildcardTakesNoneOrMany) {
    compile(kWildcards);
    EXPECT_EQ(understood("turn on"), "turn_on") << "none";
    EXPECT_EQ(understood("turn the big fan on"), "turn_on") << "many";
}

TEST_F(PatternMatchingTest, OneOrMoreWildcardNeedsAWord) {
    compile(kWildcards);
    EXPECT_EQ(understood("call my brother back"), "call_back");
    EXPECT_EQ(understood("call back"), "") << "+ needs at least one word";
    EXPECT_EQ(understood("remind me to stretch"), "remind") << "_ at the end: a word after";
    EXPECT_EQ(understood("remind me to"), "") << "nothing after";
}

TEST_F(PatternMatchingTest, ASpecificExampleBeatsAWildcardOne) {
    compile(kWildcards);
    EXPECT_EQ(understood("turn the radio on"), "specific") << "4 words found beat \"turn * on\"'s 2";
    EXPECT_EQ(understood("turn the light on"), "light_on") << "3 words beat \"turn * on\"'s 2";
}

TEST_F(PatternMatchingTest, WildcardsNextToEntitiesAndInsideNames) {
    compile(R"agent(
start_flow: "main"
entity_types { name: "fan_level" list { values { value: "high" } values { value: "low" } } }
intents { name: "set_fan" examples: ["set _ fan to @fan_level"] }
flows { name: "main" start_page: "idle" pages { name: "idle" routes { intent: "set_fan" } } }
)agent");
    nlu::NluRequest request;
    request.text = "set the fan to high";
    request.candidates = {"set_fan"};
    request.mentions = [](const std::string& entity) { return entity == "fan_level"; };
    const auto result = _agent->rules()->understand(request);
    ASSERT_FALSE(result.intents.empty()) << "@fan_level's _ is part of its name, not a wildcard";
    EXPECT_EQ(result.intents.front().intent, "set_fan");
}

TEST_F(PatternMatchingTest, WhatReadsWordsGetsThemWithoutWildcards) {
    EXPECT_EQ(flow::withoutWildcards("turn _ light on"), "turn light on");
    EXPECT_EQ(flow::withoutWildcards("call + back *"), "call back");
    EXPECT_EQ(flow::withoutWildcards("set @fan_level"), "set @fan_level");
    EXPECT_EQ(flow::plainExamples({"turn _ light on", "turn * light on", "dim it"}, 8),
              (std::vector<std::string>{"turn light on", "dim it"})) << "each once";
}

TEST_F(PatternMatchingTest, LintOnWildcards) {
    vasdk::flow::Agent agent;
    ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(R"agent(
start_flow: "main"
intents { name: "anything" examples: ["* _"] }
flows { name: "main" start_page: "idle" pages { name: "idle" routes { intent: "anything" } } }
)agent", &agent));
    std::string error;
    EXPECT_EQ(flow::CompiledAgent::compile(agent, &error), nullptr);
    EXPECT_NE(error.find("is only wildcards"), std::string::npos) << error;

    ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(R"agent(
start_flow: "main"
intents { name: "lights" examples: ["* lights on"] }
flows { name: "main" start_page: "idle" pages { name: "idle" routes { intent: "lights" } } }
)agent", &agent));
    std::vector<std::string> warnings;
    ASSERT_NE(flow::CompiledAgent::compile(agent, &error, &warnings), nullptr) << error;
    EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(),
                            [](const std::string& w) { return w.find("a * at its start or end adds nothing") != std::string::npos; }));
}

} // namespace
