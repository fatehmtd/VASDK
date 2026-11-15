// Bounded NUMBERs and ENUMs in an agent's session params, through a conversation:
// - a NUMBER's min, max and step, as expressions read when used (a tool may give the max);
// - a set outside them: ALLOW keeps it, CLAMP takes the nearest bound, REJECT keeps the old value
//   and raises out-of-range (its handler, or the engine's words), skipping the steps after it;
// - increase and decrease by the step or by `by`, stopping at the bound, and at-limit once there;
// - an ENUM's values in order: a synonym stored as its value, an unknown one refused, up and down
//   along them, wrap for modes, its attributes for a device;
// - $bounds and index_of; and what the agent can't load.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <string>

using namespace va;
using namespace va::flow;

namespace {

class BoundedValuesTest : public testing::Test {
protected:
    void start(const std::string& text) {
        vasdk::flow::Agent agent;
        ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(text, &agent)) << text;
        std::string error;
        _agent = CompiledAgent::compile(agent, &error);
        ASSERT_NE(_agent, nullptr) << error;
        _conversation = std::make_unique<FlowRuntime>(_agent);
        _conversation->start();
    }
    std::string loadError(const std::string& text) {
        vasdk::flow::Agent agent;
        EXPECT_TRUE(google::protobuf::TextFormat::ParseFromString(text, &agent)) << text;
        std::string error;
        EXPECT_EQ(CompiledAgent::compile(agent, &error), nullptr) << "should not load";
        return error;
    }
    Reply say(const std::string& words) { return _conversation->handleText(words); }
    double number(const std::string& param) const { return _conversation->sessionParam(param).number_value(); }
    std::string text(const std::string& param) const { return _conversation->sessionParam(param).string_value(); }

    std::shared_ptr<const CompiledAgent> _agent;
    std::unique_ptr<FlowRuntime> _conversation;
};

// The volume: 0 to the max a tool gives ($session.audio.max), step 2. "set to N" clamps.
constexpr const char* kVolume = R"agent(
start_flow: "main"
session_params { name: "audio" type: STRUCT default { struct_value { fields { key: "max" value { number_value: 15 } } } } }
session_params { name: "volume" type: NUMBER default { number_value: 10 } min: "0" max: "$session.audio.max" step: "2"
                 out_of_range: CLAMP }
session_params { name: "strict" type: NUMBER default { number_value: 5 } min: "1" max: "10" out_of_range: REJECT }
session_params { name: "loose" type: NUMBER default { number_value: 5 } min: "1" max: "10" }
intents { name: "louder" examples: ["louder"] }
intents { name: "quieter" examples: ["quieter"] }
intents { name: "much_louder" examples: ["much louder"] }
intents { name: "set_volume" examples: ["volume @sys.integer"] }
intents { name: "set_strict" examples: ["strict @sys.integer"] }
intents { name: "set_loose" examples: ["loose @sys.integer"] }
intents { name: "status" examples: ["volume status"] }
flows { name: "main" start_page: "idle" pages { name: "idle"
  routes { intent: "status" steps { say { text: "${bounds.volume.min} to ${bounds.volume.max}, step ${bounds.volume.step}, at the max: ${bounds.volume.at_max}." } } }
  routes { intent: "louder"  steps { increase { key: "$session.volume" } } steps { say { text: "Volume $session.volume." } } }
  routes { intent: "quieter" steps { decrease { key: "$session.volume" } } steps { say { text: "Volume $session.volume." } } }
  routes { intent: "much_louder" steps { increase { key: "$session.volume" by: "5" } } steps { say { text: "Volume $session.volume." } } }
  routes { intent: "set_volume" entity: "@sys.integer" steps { set { key: "$session.volume" value: "$entity" } }
           steps { say { text: "Volume $session.volume." } } }
  routes { intent: "set_strict" entity: "@sys.integer" steps { set { key: "$session.strict" value: "$entity" } }
           steps { say { text: "Strict $session.strict." } } }
  routes { intent: "set_loose" entity: "@sys.integer" steps { set { key: "$session.loose" value: "$entity" } }
           steps { say { text: "Loose $session.loose." } } }
  handlers { event: "at-limit" steps { branch { if: "$bounds.volume.at_max" then { say { text: "That's as loud as it goes." } }
                                                else { say { text: "It's off already." } } } } }
} }
)agent";

TEST_F(BoundedValuesTest, IncreaseAndDecreaseByTheStepWithinTheBounds) {
    start(kVolume);
    EXPECT_EQ(say("louder").text, "Volume 12.");
    EXPECT_EQ(say("louder").text, "Volume 14.");
    EXPECT_EQ(say("louder").text, "Volume 15.") << "14 + 2 stops at the max, 15";
    const auto atMax = say("louder");
    EXPECT_EQ(atMax.event, kEventAtLimit);
    EXPECT_EQ(atMax.text, "That's as loud as it goes.") << "the page's at-limit handler; \"Volume\" isn't said";
    EXPECT_EQ(number("volume"), 15);
    EXPECT_EQ(say("quieter").text, "Volume 13.");
}

TEST_F(BoundedValuesTest, ByOverridesTheStep) {
    start(kVolume);
    EXPECT_EQ(say("much louder").text, "Volume 15.") << "10 + 5";
    EXPECT_EQ(say("much louder").event, kEventAtLimit);
}

TEST_F(BoundedValuesTest, BoundsAreReadWhenUsed) {
    start(kVolume);
    Value audio;
    (*audio.mutable_struct_value()->mutable_fields())["max"].set_number_value(25);
    ASSERT_TRUE(_conversation->setSessionParam("audio", audio));
    EXPECT_EQ(say("volume 20").text, "Volume 20.") << "the max a tool just gave";
    EXPECT_EQ(say("volume 40").text, "Volume 25.") << "clamped to it";
}

TEST_F(BoundedValuesTest, ClampTakesTheNearestBound) {
    start(kVolume);
    EXPECT_EQ(say("volume 50").text, "Volume 15.");
    EXPECT_EQ(say("volume 7").text, "Volume 7.");
}

TEST_F(BoundedValuesTest, RejectKeepsTheValueAndRaisesOutOfRange) {
    start(kVolume);
    const auto refused = say("strict 50");
    EXPECT_EQ(refused.event, kEventOutOfRange);
    EXPECT_EQ(refused.text, "That goes from 1 to 10.") << "no handler: the engine's words; the steps after it don't run";
    EXPECT_EQ(number("strict"), 5) << "the value it had";
    EXPECT_EQ(say("strict 8").text, "Strict 8.");
}

TEST_F(BoundedValuesTest, AllowKeepsTheValueAsGiven) {
    start(kVolume);
    EXPECT_EQ(say("loose 50").text, "Loose 50.") << "out_of_range unset is ALLOW: bounds only for increase and decrease";
}

TEST_F(BoundedValuesTest, BoundsAreReadInReplies) {
    start(kVolume);
    EXPECT_EQ(say("volume status").text, "0 to 15, step 2, at the max: false.");
    say("volume 15");
    EXPECT_EQ(say("volume status").text, "0 to 15, step 2, at the max: true.");
}

// Screen brightness: levels in order, synonyms, attributes for the device; and fan modes that wrap.
constexpr const char* kLevels = R"agent(
start_flow: "main"
entity_types { name: "level" list {
  values { value: "dim"  synonyms: ["dark", "lowest"] attributes { fields { key: "percent" value { number_value: 5 } } } }
  values { value: "low"  attributes { fields { key: "percent" value { number_value: 25 } } } }
  values { value: "mid"  synonyms: ["medium", "half"] attributes { fields { key: "percent" value { number_value: 60 } } } }
  values { value: "high" synonyms: ["full", "max"] attributes { fields { key: "percent" value { number_value: 100 } } } } } }
entity_types { name: "mode" list { values { value: "auto" } values { value: "cool" } values { value: "heat" } } }
session_params { name: "brightness" type: ENUM entity: "@level" default { string_value: "mid" } }
session_params { name: "mode" type: ENUM entity: "@mode" default { string_value: "auto" } wrap: true }
session_params { name: "picky" type: ENUM entity: "@level" default { string_value: "low" } out_of_range: REJECT }
intents { name: "brighter" examples: ["brighter"] }
intents { name: "dimmer" examples: ["dimmer"] }
intents { name: "set_brightness" examples: ["brightness @level", "brightness to anything"] }
intents { name: "next_mode" examples: ["next mode"] }
intents { name: "previous_mode" examples: ["previous mode"] }
intents { name: "picky" examples: ["picky"] }
flows { name: "main" start_page: "idle" pages { name: "idle"
  routes { intent: "brighter" steps { increase { key: "$session.brightness" } }
           steps { say { text: "Brightness $session.brightness, ${bounds.brightness.attributes.percent} percent." } } }
  routes { intent: "dimmer" steps { decrease { key: "$session.brightness" } } steps { say { text: "Brightness $session.brightness." } } }
  routes { intent: "set_brightness" entity: "@level" steps { set { key: "$session.brightness" value: "$entity" } }
           steps { say { text: "Brightness $session.brightness, level ${$bounds.brightness.index + 1} of ${bounds.brightness.values|count}." } } }
  routes { intent: "set_brightness" steps { set { key: "$session.brightness" value: "'blinding'" } }
           steps { say { text: "Brightness $session.brightness." } } }
  routes { intent: "next_mode" steps { increase { key: "$session.mode" } } steps { say { text: "Mode $session.mode." } } }
  routes { intent: "previous_mode" steps { decrease { key: "$session.mode" } } steps { say { text: "Mode $session.mode." } } }
  routes { intent: "picky" steps { set { key: "$session.picky" value: "'blinding'" } } steps { say { text: "Set." } } }
} }
)agent";

TEST_F(BoundedValuesTest, AnEnumMovesAlongItsValues) {
    start(kLevels);
    EXPECT_EQ(say("brighter").text, "Brightness high, 100 percent.");
    const auto atMax = say("brighter");
    EXPECT_EQ(atMax.event, kEventAtLimit);
    EXPECT_EQ(atMax.text, "It's already at high.") << "no handler: the engine's words";
    EXPECT_EQ(say("dimmer").text, "Brightness mid.");
    say("dimmer");
    say("dimmer");
    EXPECT_EQ(text("brightness"), "dim");
    EXPECT_EQ(say("dimmer").event, kEventAtLimit);
}

TEST_F(BoundedValuesTest, ASynonymIsStoredAsItsValue) {
    start(kLevels);
    EXPECT_EQ(say("brightness half").text, "Brightness mid, level 3 of 4.");
    EXPECT_EQ(say("brightness dark").text, "Brightness dim, level 1 of 4.");
    std::string error;
    EXPECT_TRUE(_conversation->setSessionParam("brightness", stringValue("FULL"), &error)) << error;
    EXPECT_EQ(text("brightness"), "high") << "from the app too, any case";
}

TEST_F(BoundedValuesTest, AValueNotAmongTheEnumsIsRefused) {
    start(kLevels);
    EXPECT_EQ(say("brightness to anything").text, "Brightness mid.") << "ALLOW: refused, logged, the value it had";
    std::string error;
    EXPECT_FALSE(_conversation->setSessionParam("brightness", stringValue("blinding"), &error));
    EXPECT_NE(error.find("isn't one of"), std::string::npos) << error;
    const auto refused = say("picky");
    EXPECT_EQ(refused.event, kEventOutOfRange) << "REJECT: the event";
    EXPECT_EQ(refused.text, "That goes from dim to high.");
    EXPECT_EQ(text("picky"), "low");
}

TEST_F(BoundedValuesTest, WrapGoesRound) {
    start(kLevels);
    EXPECT_EQ(say("next mode").text, "Mode cool.");
    EXPECT_EQ(say("next mode").text, "Mode heat.");
    EXPECT_EQ(say("next mode").text, "Mode auto.") << "past the last: the first";
    EXPECT_EQ(say("previous mode").text, "Mode heat.") << "before the first: the last";
}

TEST_F(BoundedValuesTest, IndexOfFindsAValueInAList) {
    start(kLevels);
    const auto expression = Expression::parse("index_of($bounds.brightness.values, 'high')");
    ASSERT_NE(expression, nullptr);
    const Lookup none = [](const Path&) { return nullValue(); };
    Value list;
    for (const char* value : {"dim", "low", "mid", "high"}) *list.mutable_list_value()->add_values() = stringValue(value);
    const Lookup values = [&](const Path&) { return list; };
    EXPECT_EQ(expression->evaluate(values).number_value(), 3);
    EXPECT_EQ(Expression::parse("index_of($bounds.brightness.values, 'blinding')")->evaluate(values).kind_case(), Value::kNullValue);
    EXPECT_EQ(expression->evaluate(none).kind_case(), Value::kNullValue) << "not a list";
}

TEST_F(BoundedValuesTest, MistakesDontLoad) {
    const std::string page = "flows { name: \"main\" start_page: \"idle\" pages { name: \"idle\" } }\nstart_flow: \"main\"\n";
    EXPECT_NE(loadError(page + "session_params { name: \"v\" type: NUMBER min: \"10\" max: \"1\" }").find("min is more than max"),
              std::string::npos);
    EXPECT_NE(loadError(page + "session_params { name: \"v\" type: NUMBER step: \"0\" }").find("step must be more than 0"),
              std::string::npos);
    EXPECT_NE(loadError(page + "session_params { name: \"v\" type: NUMBER max: \"$session.nothing\" }").find("no session param"),
              std::string::npos)
        << "a bound's references are checked";
    EXPECT_NE(loadError(page + "session_params { name: \"v\" type: STRING min: \"1\" }").find("are for a NUMBER"), std::string::npos);
    EXPECT_NE(loadError(page + "session_params { name: \"v\" type: ENUM }").find("an ENUM needs `entity`"), std::string::npos);
    const std::string level = "entity_types { name: \"level\" list { values { value: \"low\" } values { value: \"high\" } } }\n";
    EXPECT_NE(loadError(page + level + "session_params { name: \"v\" type: ENUM entity: \"@level\" out_of_range: CLAMP }")
                  .find("CLAMP is for a NUMBER"),
              std::string::npos);
    EXPECT_NE(loadError(page + level + "session_params { name: \"v\" type: ENUM entity: \"@level\" default { string_value: \"mid\" } }")
                  .find("isn't one of @level's values"),
              std::string::npos);
    EXPECT_NE(loadError("start_flow: \"main\"\nsession_params { name: \"t\" type: STRING }\n"
                        "flows { name: \"main\" start_page: \"idle\" pages { name: \"idle\" on_enter { increase { key: \"$session.t\" } } } }")
                  .find("only a NUMBER or an ENUM goes up and down"),
              std::string::npos);
    EXPECT_NE(loadError(page + "session_params { name: \"t\" type: STRING }\n"
                               "intents { name: \"x\" examples: [\"x\"] }\n"
                               "flows { name: \"other\" start_page: \"p\" pages { name: \"p\" routes { intent: \"x\" steps { say { text: \"${bounds.t.max}\" } } } } }")
                  .find("$bounds reads a NUMBER or ENUM param"),
              std::string::npos);
}

} // namespace
