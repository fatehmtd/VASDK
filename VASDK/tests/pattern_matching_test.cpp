// Patterns as the rules understand them: what an agent's examples spell is matched like any
// example. The longest sentence found wins between intents, so "turn on [the] light" beats "turn
// on" for "turn on the light"; negation and negative examples still rule an intent out; the
// built-in intents' phrases in a language pack may be patterns too.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/nlu/RuleNluProvider.h>

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <string>

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

} // namespace
