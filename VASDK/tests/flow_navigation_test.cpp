// Flow navigation: "flow:<name>" remembers where it came from and END_FLOW goes back there, asking
// that page's question again; a digression from a form keeps its values; END_FLOW in the start
// flow ends the conversation; the depth cap; END_SESSION forgets everything; undo by copy.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <cstdio>
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
intents { name: "book" examples: ["book"] }
intents { name: "hours" examples: ["hours"] }
intents { name: "deeper" examples: ["deeper"] }
intents { name: "back" examples: ["back"] }
intents { name: "bye" examples: ["bye"] }
envelopes: ["global"]
envelope_defs {
  name: "global"
  routes { intent: "hours" target: "flow:hours" }
  routes { intent: "back" target: "END_FLOW" }
  routes { intent: "bye" target: "END_SESSION" }
}
goals {
  name: "booking"
  slots { name: "day" type: ENUM values: ["monday", "tuesday"] prompt: "Which day?" }
  slots { name: "phone" type: PHONE prompt: "Your number?" }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Book or hours?" } } }
    routes { intent: "book" target: "booking" }
    routes { intent: "deeper" target: "flow:deep" }
  }
  pages {
    name: "booking"
    form { goal: "booking" }
    routes { condition: "$form.complete" steps { say { text: "Booked for $form.day." } } target: "welcome" }
  }
}
flows {
  name: "hours"
  start_page: "answer"
  pages { name: "answer" on_enter { say { text: "We're open 8 to 6." } } on_enter { goto { target: "END_FLOW" } } }
}
flows {
  name: "deep"
  start_page: "level"
  pages { name: "level" on_enter { ask { say { text: "Deeper or back?" } } } routes { intent: "deeper" target: "flow:deep" } }
}
)agent";

} // namespace

int main() {
    vasdk::flow::Agent definition;
    google::protobuf::TextFormat::ParseFromString(kAgent, &definition);
    std::string error;
    auto agent = CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the navigation agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    // A digression from the start page: the hours flow answers and goes back.
    FlowRuntime conversation(agent);
    conversation.start();
    auto reply = conversation.handleText("what are your hours");
    check(reply.text == "We're open 8 to 6. Book or hours?" && conversation.pageName() == "welcome",
          "into the hours flow and back, with the question again");
    check(reply.pageChanges.size() == 2 && reply.pageChanges[0].toFlow == "hours" &&
              reply.pageChanges[1].reason == "return" && reply.pageChanges[1].toPage == "welcome",
          "page changes: into the flow, then return");

    // The same digression from a half-filled form: its values are kept and it asks what's next.
    conversation.handleText("book");
    reply = conversation.handleText("monday");
    check(reply.text == "Your number?", "the form asks for the phone");
    reply = conversation.handleText("hours please");
    check(reply.text == "We're open 8 to 6. Your number?" && conversation.pageName() == "booking" &&
              toText(conversation.lookup({"form", "day"})) == "monday",
          "back to the booking: the day kept, the phone asked again");
    reply = conversation.handleText("555 123 4567");
    check(contains(reply.text, "Booked for monday.") && conversation.pageName() == "welcome", "the booking completes");

    // END_FLOW in the start flow: nothing to go back to, the conversation ends.
    reply = conversation.handleText("back");
    check(reply.endsSession && conversation.ended(), "END_FLOW in the start flow ends the conversation");

    // Flows within flows go back one at a time; past the cap the way back isn't kept.
    FlowRuntime deep(agent);
    deep.start();
    for (int i = 0; i < 10; ++i) deep.handleText("deeper");
    int returns = 0;
    while (!deep.ended() && returns < 20) {
        deep.handleText("back");
        if (!deep.ended()) ++returns;
    }
    check(returns == static_cast<int>(FlowRuntime::kMaxFlowDepth), "back as many times as flows were remembered");

    // END_SESSION forgets the way back; the next conversation starts fresh.
    FlowRuntime ending(agent);
    ending.start();
    ending.handleText("deeper");
    reply = ending.handleText("bye");
    check(reply.endsSession, "END_SESSION from inside a flow");
    ending.handleText("hello");   // starts over on the start page
    reply = ending.handleText("back");
    check(reply.endsSession, "nothing remembered after the end");

    // Undo: a copy keeps its own way back.
    FlowRuntime original(agent);
    original.start();
    original.handleText("deeper");
    const FlowRuntime before = original;
    original.handleText("back");
    FlowRuntime restored = before;
    reply = restored.handleText("back");
    check(original.flowName() == "main" && restored.flowName() == "main" && !restored.ended() &&
              reply.text == "Book or hours?",
          "a copy goes back on its own");

    if (failures == 0) std::printf("flow_navigation_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
