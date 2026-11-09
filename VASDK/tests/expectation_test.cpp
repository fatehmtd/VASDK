// Expected input: every reply says what the user should say next, derived from the question (an
// ask, a form slot, the read-back) and changed by an `expect`. It narrows the routes matched,
// sets the no-input timer and reprompt limit, reprompts a partial digit answer, reads digits for
// a plain slot, offers a slot's few values as choices (selectChoice taps one), and sets how long
// the mic stays open: a say's own value, the question's, the page's envelopes', the agent's, then
// the engine's (AgentStateProcessor), kept open at least until a question's no-input reprompt.

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/DialogProcessor.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace va::flow;
using namespace std::chrono_literals;
using Expectation = vasdk::flow::Expectation;

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
intents { name: "sell" examples: ["sell"] }
intents { name: "secure" examples: ["secure"] }
intents { name: "person" examples: ["person"] }
envelopes: ["standard"]
envelope_defs {
  name: "standard"
  routes { intent: "person" steps { say { text: "Putting you through." } } }
  settings { follow_up_listen_ms: 5000 }
}
envelope_defs { name: "patient" settings { follow_up_listen_ms: 9000 } }
goals {
  name: "booking"
  require_confirmation: true
  confirm: "Book it for $form.day?"
  slots { name: "day" type: ENUM values: ["monday", "tuesday"] prompt: "Which day?" expect { no_input_ms: 4000 } }
  slots { name: "cars" type: INTEGER prompt: "How many cars?" }
  slots { name: "phone" type: PHONE prompt: "Your number?" expect { no_input_ms: 12000 max_reprompts: 5 } }
  slots { name: "card" type: STRING prompt: "Your card number?"
          expect { min_digits: 16 max_digits: 16 secure_entry: true follow_up_listen_ms: 15000 mode: DIGITS keyboard: NUMERIC
                   partial_reprompt: "I got $input.digit_count digits. Cards have 16." } }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Book or hours?" }
                     choices { label: "Book" value: "book" } choices { label: "Hours" value: "hours" }
                     expect { intents: ["book", "hours", "secure"] keyboard: TEXT } } }
    routes { intent: "book" target: "booking" }
    routes { intent: "hours" steps { say { text: "Open 8 to 6." follow_up_listen_ms: 3000 } } }
    routes { intent: "sell" steps { say { text: "We don't sell cars." } } }
    routes { intent: "secure" target: "secure" }
  }
  pages {
    name: "secure"
    on_enter { ask { say { text: "Say the code." } expect { allow_global: false } } }
    routes { intent: "book" target: "booking" }
  }
  pages { name: "booking" envelopes: ["patient"] form { goal: "booking" } routes { condition: "$form.confirmed" target: "done" } }
  pages { name: "done" on_enter { say { text: "Booked." } } on_enter { end {} } }
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

// Records what reaches it: TextFrames, FollowUpListenFrames and agent states.
class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame)) texts.push_back(*text);
        if (auto window = std::dynamic_pointer_cast<transport::FollowUpListenFrame>(frame)) windows.push_back(*window);
        if (auto state = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame)) states.push_back(*state);
    }
    void settle() { std::this_thread::sleep_for(50ms); }
    std::mutex mutex;
    std::vector<transport::TextFrame> texts;
    std::vector<transport::FollowUpListenFrame> windows;
    std::vector<transport::AgentStateFrame> states;
};

void start(pipeline::FrameProcessor& processor) {
    processor.initialize();
    processor.start();
}

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    // Derived from the question, with the ask's expect on top; the agent's envelope sets the window.
    FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.expectation.mode() == Expectation::CHOICE && reply.expectation.keyboard() == Expectation::TEXT,
          "an ask with choices: CHOICE, with the expect's keyboard");
    check(reply.followUpListenMs == 5000, "level 3: the agent's envelope");

    // The question's intents: a page route outside them isn't matched; envelope routes still are.
    reply = conversation.handleText("do you sell cars");
    check(!reply.matched && reply.text == "Sorry, I can't help with that here. Book or hours?",
          "an intent the question doesn't expect: not understood");
    reply = conversation.handleText("a person please");
    check(reply.matched && contains(reply.text, "Putting you through."), "the envelope's routes still match");
    reply = conversation.handleText("hours");
    check(reply.text == "Open 8 to 6. Book or hours?" && reply.followUpListenMs == 3000,
          "level 5: a say's own window comes before all");
    check(reply.expectation.mode() == Expectation::CHOICE && reply.choices.size() == 2, "asked again: the same expectation");

    // allow_global: false keeps the envelope's routes out.
    reply = conversation.handleText("secure");
    check(conversation.pageName() == "secure" && reply.expectation.mode() == Expectation::OPEN, "an ask without choices: OPEN");
    reply = conversation.handleText("a person please");
    check(!reply.matched, "allow_global false: the envelope's routes don't match");

    // Form slots: choices for few values, tapped with selectChoice; a page envelope's window.
    reply = conversation.handleText("book");
    check(conversation.pageName() == "booking" && reply.text == "Which day?" &&
              reply.expectation.mode() == Expectation::SLOT && reply.expectation.slot() == "day",
          "a slot: SLOT, named");
    check(reply.choices.size() == 2 && reply.choices[1].label == "tuesday", "an ENUM slot's few values are its choices");
    check(reply.followUpListenMs == 9000, "level 3: the page's envelope before the agent's");
    reply = conversation.selectChoice(1);
    check(conversation.lookup({"form", "day"}).string_value() == "tuesday", "a tapped choice answers the slot");
    check(reply.text == "How many cars?" && reply.expectation.keyboard() == Expectation::NUMERIC && reply.choices.empty(),
          "INTEGER: a keypad, no choices");
    reply = conversation.selectChoice(5);
    check(!reply.matched && reply.text.empty(), "a choice that wasn't offered: nothing happens");
    reply = conversation.handleText("2");

    // A PHONE slot: DIGITS from 7 to 11, its own no-input timer and reprompt limit.
    check(reply.text == "Your number?" && reply.expectation.mode() == Expectation::DIGITS &&
              reply.expectation.min_digits() == 7 && reply.expectation.max_digits() == 11 &&
              reply.expectation.keyboard() == Expectation::NUMERIC,
          "PHONE: DIGITS from 7 to 11, a keypad");
    check(conversation.noInputMs() == 12000, "the expect's no-input timer before the page's");
    reply = conversation.handleText("five five five one two");
    check(reply.text == "I only got 5 digits. Your number?" && reply.expectation.mode() == Expectation::DIGITS,
          "too few digits: said so, and asked again");
    for (int miss = 0; miss < 3; ++miss) reply = conversation.handleText("purple");
    check(conversation.pageName() == "booking" && !conversation.lookup({"form", "failed"}).bool_value(),
          "the expect's max_reprompts 5 before the goal's 2: four misses don't fail the form");
    reply = conversation.handleText("555 123 4467");

    // A STRING slot expecting DIGITS: read as its digits, 16 of them; the partial reprompt.
    check(reply.text == "Your card number?" && reply.expectation.secure_entry() && reply.followUpListenMs == 15000,
          "level 4: the question's window before the page's; secure entry");
    reply = conversation.handleText("four one one one, one one one one");
    check(reply.text == "I got 8 digits. Cards have 16." && conversation.lookup({"form", "card"}).kind_case() == Value::kNullValue,
          "the partial reprompt reads $input.digit_count");
    reply = conversation.handleText("it's 4111 1111 1111 1111 thanks");
    check(conversation.lookup({"form", "card"}).string_value() == "4111111111111111", "only the digits are kept");
    check(reply.expectation.mode() == Expectation::CONFIRMATION && reply.text == "Book it for tuesday?",
          "the read-back: CONFIRMATION");
    reply = conversation.handleText("yes");
    check(reply.endsSession && reply.expectation.mode() == Expectation::NONE, "nothing asked: NONE");

    // Lint
    check(contains(lintError("intents: [\"book\", \"hours\", \"secure\"]", "intents: [\"bok\"]"), "expect: unknown intent \"bok\""),
          "an expect's intents must be declared");
    check(contains(lintError("min_digits: 16 max_digits: 16", "min_digits: 16 max_digits: 4"), "min_digits is more than max_digits"),
          "min_digits above max_digits");
    check(contains(lintError("follow_up_listen_ms: 3000", "follow_up_listen_ms: -2"), "follow_up_listen_ms must be -1"),
          "a window below -1");
    check(contains(lintError("$input.digit_count", "$input.digits"), "$input.digits"), "partial reprompts' references are checked");

    // The dialog stage: the expectation on the reply's last frame, its window sent upstream, and taps.
    {
        auto dialog = std::make_shared<DialogProcessor>(agent);
        auto downstream = std::make_shared<Recorder>();
        auto upstream = std::make_shared<Recorder>();
        dialog->setDownstream(downstream);
        dialog->setUpstream(upstream);
        start(*downstream);
        start(*upstream);
        start(*dialog);
        auto state = std::make_shared<transport::AgentStateFrame>();
        state->previousState = transport::AgentState::IDLE;
        state->currentState = transport::AgentState::LISTENING;
        state->transition = transport::AgentStateTransition::APP_COMMAND;
        dialog->processFrame(state, pipeline::FlowDirection::DOWNSTREAM);
        dialog->selectChoice(0);
        downstream->settle();
        {
            std::lock_guard<std::mutex> lock(downstream->mutex);
            Expectation expectation;
            check(!downstream->texts.empty() && expectation.ParseFromString(downstream->texts.back().expectBytes) &&
                      expectation.mode() == Expectation::SLOT && expectation.slot() == "day" &&
                      downstream->texts.back().choices.size() == 2,
                  "a tap routes; the reply's last frame carries its expectation");
        }
        {
            std::lock_guard<std::mutex> lock(upstream->mutex);
            check(upstream->windows.size() == 2 && upstream->windows[0].listenMs == 5000 &&
                      upstream->windows[0].atLeastMs == 0 && upstream->windows[1].listenMs == 9000,
                  "each reply's window goes upstream");
            check(upstream->windows.size() == 2 && upstream->windows[1].atLeastMs == 4000 + 2000,
                  "a question with a no-input timer: open until its reprompt has started");
        }
        dialog->stop();
        downstream->stop();
        upstream->stop();
    }

    // Levels 1-2 and the reply's own window, in AgentStateProcessor.
    {
        auto agentState = std::make_shared<pipeline::AgentStateProcessor>();
        auto recorder = std::make_shared<Recorder>();
        pipeline::BackchannelConfig hardBargeIn;
        hardBargeIn.enabled = false;
        agentState->setBackchannelConfig(hardBargeIn);
        agentState->setAlwaysOn(false);
        agentState->setFollowUpListenMs(150);   // the engine's: its config, or setFollowUpListenMs
        agentState->setDownstream(recorder);
        start(*recorder);
        start(*agentState);
        using Turn = transport::TurnStateFrame;
        auto send = [&](Turn::TurnActor actor, Turn::TurnState turnState) {
            agentState->processFrame(std::make_shared<Turn>(actor, turnState, Turn::TurnType::NORMAL),
                                     pipeline::FlowDirection::DOWNSTREAM);
        };
        auto exchange = [&](std::optional<int> listenMs, int atLeastMs) -> int {
            size_t before = 0;
            {
                std::lock_guard<std::mutex> lock(recorder->mutex);
                before = recorder->states.size();
            }
            send(Turn::TurnActor::USER, Turn::TurnState::START);
            send(Turn::TurnActor::USER, Turn::TurnState::END);
            if (listenMs || atLeastMs) {
                auto window = std::make_shared<transport::FollowUpListenFrame>();
                window->listenMs = listenMs;
                window->atLeastMs = atLeastMs;
                agentState->processFrame(window, pipeline::FlowDirection::UPSTREAM);
            }
            send(Turn::TurnActor::SYSTEM, Turn::TurnState::START);
            send(Turn::TurnActor::SYSTEM, Turn::TurnState::END);
            // The state that opens the follow-up window, the only one carrying it. Waited for, not
            // read after a fixed time: on a slow machine it comes late, or the 150 ms window has
            // already closed and a later state follows it.
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(recorder->mutex);
                    for (size_t i = before; i < recorder->states.size(); ++i)
                        if (recorder->states[i].listenWindowMs != 0) return recorder->states[i].listenWindowMs;
                }
                std::this_thread::sleep_for(5ms);
            }
            return -99;
        };
        auto wake = std::make_shared<transport::TriggerFrame>();
        wake->triggerType = transport::TriggerType::WAKE_WORD;
        agentState->processFrame(wake, pipeline::FlowDirection::DOWNSTREAM);
        check(exchange(std::nullopt, 0) == 150, "levels 1-2: the engine's window when the reply has none");
        check(exchange(400, 0) == 400, "the reply's own window");
        check(exchange(std::nullopt, 0) == 150, "used once: the next reply has the engine's again");
        check(exchange(std::nullopt, 600) == 600, "a question: at least until its no-input reprompt");
        check(exchange(-1, 600) == -1, "-1 still never closes");
        agentState->stop();
        recorder->stop();
    }

    if (failures == 0) std::printf("expectation_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
