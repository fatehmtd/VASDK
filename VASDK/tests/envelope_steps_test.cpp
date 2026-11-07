// Envelopes in an agent file: guards (on entering a page and on a turn there, not in the flow they
// send the user to, END_FLOW back), before steps (extract a value said anywhere, count turns, a
// goto that answers the turn) and after steps (added to every reply). Expressions and filters in
// replies against the runtime's clock: arithmetic, days_until, |raw, |plural. And lint errors.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <cstdio>
#include <ctime>
#include <string>

using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kAgent = R"agent(
start_flow: "main"
session_params { name: "verified" type: BOOL default { bool_value: false } }
session_params { name: "turns" type: NUMBER default { number_value: 0 } }
session_params { name: "plate" type: STRING }
session_params { name: "visit" type: STRING default { string_value: "2026-11-10" } }
session_params { name: "cars" type: LIST }
intents { name: "balance" examples: ["balance", "my balance"] }
intents { name: "code" examples: ["the code is 1234", "1234"] }
intents { name: "visit" examples: ["when is my visit"] }
intents { name: "plate" examples: ["what plate"] }
intents { name: "bye" examples: ["bye"] }
intents { name: "lock" examples: ["lock it"] }
entity_types { name: "plate" pattern { regex: "[a-z]{3}[0-9]{3}" uppercase: true } }
envelopes: ["counted"]
envelope_defs {
  name: "counted"
  before { set { key: "$session.turns" value: "$session.turns + 1" } }
  before { extract { entity: "@plate" into: "$session.plate" } }
  before { branch { if: "$input.text : 'goodbye'" then { say { text: "Goodbye." } } then { end {} } } }
  after { branch { if: "$session.turns > 0 AND $session.turns % 3 = 0"
                   then { say { text: "(${session.turns|plural:'turn':'turns'} so far.)" } } } }
}
envelope_defs {
  name: "secure"
  guards { condition: "$session.verified" steps { say { text: "Please verify first." } } target: "flow:verify" }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Hi." } } }
    routes { intent: "balance" target: "flow:account" }
    routes { intent: "visit" steps { say { text: "Your visit is ${session.visit|raw}, in ${days_until($session.visit)} days, ${$session.turns * 10} points." } } }
    routes { intent: "plate" steps { say { text: "Plate ${session.plate|default:'unknown'}." } } }
  }
}
flows {
  name: "account"
  start_page: "balance"
  envelopes: ["secure"]
  pages {
    name: "balance"
    on_enter { ask { say { text: "You have $$42. Anything else?" } } }
    routes { intent: "lock" steps { set { key: "$session.verified" value: "false" } } steps { say { text: "Locked." } } }
  }
}
flows {
  name: "verify"
  start_page: "ask_code"
  envelopes: ["secure"]
  pages {
    name: "ask_code"
    on_enter { ask { say { text: "What's your code?" } } }
    routes { intent: "code" steps { set { key: "$session.verified" value: "true" } } steps { say { text: "Thanks." } } target: "flow:account" }
  }
}
)agent";

std::shared_ptr<const CompiledAgent> compileText(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error);
}

std::string lintError(const std::string& replace, const std::string& with) {
    std::string text = kAgent;
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    text.replace(at, replace.size(), with);
    std::string error;
    return compileText(text, &error) ? "" : error;
}

// 2026-11-05, noon, local time.
std::chrono::system_clock::time_point thursdayNoon() {
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 10;
    local.tm_mday = 5;
    local.tm_hour = 12;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    FlowRuntime runtime(agent);
    runtime.setClock(thursdayNoon);
    auto reply = runtime.start();
    check(reply.text == "Hi.", "the greeting");

    // Before: counted, and a plate said in passing is kept.
    reply = runtime.handleText("when is my visit");
    check(reply.text == "Your visit is 2026-11-10, in 5 days, 10 points. Hi.", "|raw, days_until and arithmetic in a reply");
    if (!contains(reply.text, "in 5 days")) std::fprintf(stderr, "  reply: %s\n", reply.text.c_str());
    reply = runtime.handleText("I drive abc123, what plate");
    check(contains(reply.text, "Plate ABC123."), "a before step extracted the plate said anywhere");
    if (!contains(reply.text, "Plate ABC123.")) std::fprintf(stderr, "  reply: %s\n", reply.text.c_str());
    check(runtime.sessionParam("turns").number_value() == 2, "a before step counted the turns");

    // After: added to every third reply.
    reply = runtime.handleText("blah");
    check(contains(reply.text, "(3 turns so far.)"), "an after step adds to the reply");

    // A guard on entering: not verified, so the account flow sends the user to verify.
    reply = runtime.handleText("my balance");
    check(contains(reply.text, "Please verify first.") && contains(reply.text, "What's your code?") && !contains(reply.text, "$42"),
          "a guard keeps the user out, and sends them to its target");
    if (!contains(reply.text, "What's your code?")) std::fprintf(stderr, "  reply: %s\n", reply.text.c_str());
    check(runtime.flowName() == "verify", "into the guard's flow, where it doesn't apply");
    reply = runtime.handleText("1234");
    check(contains(reply.text, "Thanks.") && contains(reply.text, "You have $42."), "verified: the guarded page");
    check(runtime.pageName() == "balance", "on the guarded page");

    // A guard on a turn: the condition stopped holding while on the page.
    reply = runtime.handleText("lock it");
    check(contains(reply.text, "Locked."), "the route ran");
    reply = runtime.handleText("my balance");
    check(contains(reply.text, "Please verify first.") && runtime.flowName() == "verify", "a guard on a turn there");

    // A before step that moves answers the turn.
    reply = runtime.handleText("goodbye now");
    check(reply.text == "Goodbye." && reply.endsSession, "a before step that ends answers the turn");

    // Lint
    check(contains(lintError("after { branch { if: \"$session.turns > 0", "after { goto { target: \"END_SESSION\" } }\n  after { branch { if: \"$session.turns > 0"),
                   "an after step can't move"),
          "an after step can't move");
    check(contains(lintError("condition: \"$session.verified\"", "condition: \"$session.verify\""), "no session param \"verify\""),
          "a guard's condition is checked");
    check(contains(lintError("target: \"flow:verify\" }", "target: \"flow:nowhere\" }"), "unknown target"),
          "a guard's target is checked");
    check(contains(lintError("${days_until($session.visit)}", "${days_until($session.visit)|loud}"), "unknown filter 'loud'"),
          "a reply's filters are checked");
    check(contains(lintError("${$session.turns * 10}", "${$session.turn * 10}"), "no session param \"turn\""),
          "references in a reply's expression are checked");

    if (failures == 0) std::printf("envelope_steps_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
