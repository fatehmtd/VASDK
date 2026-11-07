// Envelopes: a page's own routes before its envelopes, its flow's and the agent's; extends; inner
// handlers and settings over outer ones; the no-match, no-input and max-reprompts events and their
// built-in behaviour; routes that stay re-asking the question; and lint errors.

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
intents { name: "help" examples: ["help"] }
intents { name: "person" examples: ["a person"] }
intents { name: "monday" examples: ["monday"] }
envelopes: ["standard"]
envelope_defs {
  name: "standard"
  routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
  routes { intent: "help" steps { say { text: "Global help." } } }
  routes { intent: "person" target: "flow:handoff" }
  handlers { event: "no-match" steps { say { text: "Sorry, I didn't get that." } } }
  handlers { event: "no-input" steps { say { text: "Are you still there?" } } }
  handlers { event: "max-reprompts" steps { say { text: "Let me get someone." } } target: "flow:handoff" }
  settings { max_reprompts: 2 no_input_ms: 5000 }
}
envelope_defs {
  name: "booking"
  extends: "standard"
  handlers { event: "no-match" steps { ask { say { text: "Sorry, which day: Monday or Tuesday?" } } } }
  settings { max_reprompts: 1 }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Book or hours?" } } }
    routes { intent: "book" target: "flow:booking" }
    routes { intent: "help" steps { say { text: "Page help: say book." } } }
  }
}
flows {
  name: "booking"
  start_page: "day"
  envelopes: ["booking"]
  pages { name: "day" on_enter { ask { say { text: "Which day?" } } } routes { intent: "monday" target: "END_FLOW" } }
}
flows {
  name: "handoff"
  start_page: "transfer"
  pages { name: "transfer" on_enter { say { text: "Connecting." } } on_enter { end {} } }
}
)agent";

const char* kPlainAgent = R"agent(
start_flow: "main"
intents { name: "hours" examples: ["hours"] }
flows {
  name: "main"
  start_page: "welcome"
  pages { name: "welcome" on_enter { ask { say { text: "How can I help?" } } }
          routes { intent: "hours" steps { say { text: "8 to 6." } } } }
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

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the envelope agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    // Routes: the page's own first, then the agent's envelope; a route that stays asks again.
    FlowRuntime conversation(agent);
    conversation.start();
    auto reply = conversation.handleText("help");
    check(reply.text == "Page help: say book. Book or hours?", "the page's route wins over the envelope's");
    reply = conversation.handleText("what are your hours");
    check(reply.text == "We're open 8 to 6. Book or hours?" && conversation.pageName() == "welcome",
          "an envelope route, then the page's question again");

    // A flow's envelope that extends the agent's: its routes and handlers, then the base's.
    conversation.handleText("book");
    reply = conversation.handleText("hours?");
    check(reply.text == "We're open 8 to 6. Which day?", "a route inherited through extends");
    reply = conversation.handleText("blue");
    check(!reply.matched && reply.event == "no-match" && reply.text == "Sorry, which day: Monday or Tuesday?",
          "the inner no-match handler, which asks: no second question");
    reply = conversation.handleText("green");
    check(reply.event == "max-reprompts" && contains(reply.text, "Let me get someone. Connecting.") && reply.endsSession,
          "the inner max_reprompts (1): the second miss raises max-reprompts, inherited from the base");

    // The agent's settings on its own pages: max_reprompts 2, and a reset after a match.
    FlowRuntime misses(agent);
    misses.start();
    reply = misses.handleText("blue");
    check(reply.event == "no-match" && reply.text == "Sorry, I didn't get that. Book or hours?",
          "no-match: the handler, then the question again");
    misses.handleText("green");
    misses.handleText("hours");   // understood: the count starts over
    misses.handleText("red");
    reply = misses.handleText("yellow");
    check(reply.event == "no-match" && misses.pageName() == "welcome", "two misses after a match are still two");
    reply = misses.handleText("purple");
    check(reply.event == "max-reprompts" && misses.flowName() == "handoff", "the third miss in a row");

    // No-input: the handler and the question; it counts toward max-reprompts too.
    FlowRuntime silent(agent);
    silent.start();
    check(silent.noInputMs() == 5000, "no_input_ms from the envelope");
    reply = silent.handleNoInput();
    check(reply.event == "no-input" && reply.text == "Are you still there? Book or hours?" && reply.expectsReply,
          "no-input: the handler, then the question again");
    silent.handleNoInput();
    reply = silent.handleText("blue");
    check(reply.event == "max-reprompts", "no-match after two no-inputs is the third miss");

    // Without envelopes: the built-in behaviour, and no no-input timer.
    auto plain = compileText(kPlainAgent, &error);
    check(plain != nullptr, "the plain agent compiles");
    if (plain) {
        FlowRuntime builtIn(plain);
        builtIn.start();
        check(builtIn.noInputMs() == 0, "no no-input handler or setting: never");
        reply = builtIn.handleText("blue");
        check(reply.event == "no-match" && reply.text == "Sorry, I can't help with that here. How can I help?",
              "built-in no-match");
        reply = builtIn.handleNoInput();
        check(reply.event == "no-input" && reply.text == "How can I help?", "built-in no-input: the question again");
        reply = builtIn.handleText("hours");
        check(reply.text == "8 to 6. How can I help?", "a route that stays asks again");
    }

    // Lint
    check(contains(lintError("envelopes: [\"booking\"]", "envelopes: [\"bookings\"]"), "unknown envelope \"bookings\""),
          "unknown envelope");
    check(contains(lintError("extends: \"standard\"", "extends: \"booking\""), "extends itself"), "an extends cycle");
    check(contains(lintError("target: \"flow:handoff\" }\n  handlers", "target: \"welcome\" }\n  handlers"),
                   "an envelope's targets are"),
          "an envelope can't target a page by name");
    check(contains(lintError("event: \"no-input\"", "event: \"silence\""), "unknown event \"silence\""), "unknown event");

    if (failures == 0) std::printf("envelope_layering_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
