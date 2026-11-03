// Scripted pages: an agent is checked when compiled (errors and warnings say where); keyword
// intents match whole phrases, the longest winning; the runtime moves between pages, repeats the
// question when not understood, stops goto loops and starts over after the end; test cases
// report what differs; DialogProcessor greets on the first listen, answers turns, undoes a resumed
// turn and passes unmatched ones to an LLM; a Session with an agent and no LLM answers by text.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>
#include <VASDK/context/ContextProvider.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/DialogProcessor.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/flow/FlowTestRunner.h>
#include "vasdk/engine/events.pb.h"

#include <chrono>
#include <cstdio>
#include <functional>

using namespace va;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

template <class Message>
Message parse(const std::string& text) {
    Message message;
    std::string error;
    if (!engine::parseProto(text, engine::ProtoFormat::Text, message, &error))
        std::fprintf(stderr, "parse error: %s\n", error.c_str());
    return message;
}

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "book"   examples: ["book", "appointment"] }
  intents { name: "cancel" examples: ["cancel", "cancel my appointment"] }
  intents { name: "yes"    examples: ["yes"] }
  intents { name: "again"  examples: ["again"] }
  flows {
    name: "main" start_page: "welcome"
    pages { name: "welcome"
            on_enter { ask { say { text: "Book or cancel?" } } }
            routes { intent: "book" target: "booking" }
            routes { intent: "cancel" steps { say { text: "Cancelled." } } target: "END_SESSION" }
            routes { intent: "again" target: "CURRENT_PAGE" } }
    pages { name: "booking"
            on_enter { say { text: "Booking." } }
            on_enter { goto { target: "confirm" } }
            on_enter { say { text: "never said" } } }
    pages { name: "confirm"
            on_enter { ask { say { text: "Confirm?" } } }
            routes { intent: "yes" steps { say { text: "Done." } } steps { end {} } } }
  }
)";

std::shared_ptr<const flow::CompiledAgent> compile(const std::string& text, std::string* error = nullptr,
                                                   std::vector<std::string>* warnings = nullptr) {
    return flow::CompiledAgent::compile(parse<vasdk::flow::Agent>(text), error, warnings);
}

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame)) texts.push_back(*text);
        if (frame->getType() == transport::FrameType::USER_TURN) ++turns;
    }
    std::vector<transport::TextFrame> settled() {
        std::this_thread::sleep_for(50ms);
        std::lock_guard<std::mutex> lock(mutex);
        return texts;
    }
    std::mutex mutex;
    std::vector<transport::TextFrame> texts;
    int turns = 0;
};

std::shared_ptr<transport::UserTurnFrame> userTurn(const std::string& text, bool replacesPrevious = false) {
    auto turn = std::make_shared<transport::UserTurnFrame>();
    turn->text = text;
    turn->replacesPrevious = replacesPrevious;
    return turn;
}

struct EventRecorder {
    std::mutex mutex;
    std::vector<vasdk::engine::Event> events;
    engine::EventListener listener() {
        return [this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
    }
    bool waitFor(const std::function<bool(const vasdk::engine::Event&)>& match) {
        for (int i = 0; i < 150; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& event : events) if (match(event)) return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
};

} // namespace

int main() {
    // Compile checks: every error, with where it is; warnings don't stop it.
    {
        std::string error;
        check(!compile(R"(
            start_flow: "main"
            intents { name: "book" examples: ["book"] }
            flows { name: "main" start_page: "welcome"
                    pages { name: "welcome" routes { intent: "bok" target: "nowhere" } }
                    pages { name: "welcome" } }
        )", &error), "a broken agent doesn't compile");
        check(contains(error, "flow \"main\" page \"welcome\" route 1: unknown intent \"bok\""), "unknown intent, located");
        check(contains(error, "route 1: unknown target \"nowhere\""), "unknown target, located");
        check(contains(error, "flow \"main\" page \"welcome\": is declared twice"), "duplicate page");
        check(!compile(R"(start_flow: "missing")", &error) && contains(error, "start_flow \"missing\""),
              "missing start flow");

        std::vector<std::string> warnings;
        check(compile(R"(
            start_flow: "main"
            intents { name: "book" examples: ["book"] }
            intents { name: "unused" examples: ["x"] }
            flows { name: "main" start_page: "a"
                    pages { name: "a" routes { intent: "book" target: "a" } }
                    pages { name: "orphan" routes { intent: "book" target: "a" } } }
        )", &error, &warnings) != nullptr, "warnings don't stop compiling");
        bool orphan = false, unused = false;
        for (const auto& warning : warnings) {
            orphan |= contains(warning, "page \"orphan\": is never reached");
            unused |= contains(warning, "intent \"unused\": is not used");
        }
        check(orphan && unused, "unreachable page and unused intent warned");
    }

    auto agent = compile(kAgent);
    check(agent != nullptr, "the test agent compiles");
    if (!agent) return 1;

    // Keyword intents: whole words, in order; the longest phrase wins between intents.
    {
        check(flow::splitWords("Book an Oil-Change, don't!") ==
                  std::vector<std::string>({"book", "an", "oil", "change", "don't"}), "words split");
        const auto words = flow::splitWords("please cancel my appointment");
        check(agent->matchIntent("cancel", words) == 3 && agent->matchIntent("book", words) == 1,
              "longest example scores");
        check(agent->matchIntent("book", flow::splitWords("bookkeeping")) == 0, "whole words only");
        flow::FlowRuntime conversation(agent);
        conversation.start();
        check(conversation.handleText("cancel my appointment").intent == "cancel", "longest phrase wins over route order");
    }

    // The runtime: greeting, goto (steps after it skipped), not understood, CURRENT_PAGE, end, start over.
    {
        flow::FlowRuntime conversation(agent);
        auto reply = conversation.start();
        check(reply.text == "Book or cancel?" && reply.expectsReply && conversation.pageName() == "welcome", "greeting");
        reply = conversation.handleText("um what");
        check(!reply.matched && reply.text == "Sorry, I can't help with that here. Book or cancel?", "not understood repeats the question");
        reply = conversation.handleText("again");
        check(reply.text == "Book or cancel?" && reply.pageChanges.size() == 1 &&
              reply.pageChanges[0].reason == "route:again", "CURRENT_PAGE enters the page again");
        reply = conversation.handleText("book");
        check(reply.text == "Booking. Confirm?" && conversation.pageName() == "confirm" &&
              reply.pageChanges.size() == 2 && reply.pageChanges[1].reason == "goto", "goto skips the steps after it");
        reply = conversation.handleText("yes");
        check(reply.text == "Done." && reply.endsSession && conversation.ended(), "end step");
        reply = conversation.handleText("book");
        check(conversation.pageName() == "confirm" && !reply.pageChanges.empty() && reply.pageChanges.front().reason == "restart",
              "after the end, the next turn starts over at the start page");

        auto loop = compile(R"(
            start_flow: "main"
            flows { name: "main" start_page: "a"
                    pages { name: "a" on_enter { say { text: "a" } } on_enter { goto { target: "b" } } }
                    pages { name: "b" on_enter { goto { target: "a" } } } }
        )");
        flow::FlowRuntime looping(loop);
        check(looping.start().pageChanges.size() == flow::FlowRuntime::kMaxTransitions, "a goto loop stops");
    }

    // Conditions and stored values: checked at load, then set/branch/switch, route conditions,
    // page locals, $context and typed session params.
    {
        std::string error;
        check(!compile(R"(
            start_flow: "main"
            session_params { name: "tier" }
            session_params { name: "count" type: NUMBER default { string_value: "x" } }
            intents { name: "go" examples: ["go"] }
            flows { name: "main" start_page: "a"
                    pages { name: "a"
                            on_enter { say { text: "Hi $session.nmae" } }
                            on_enter { set { key: "$input.text" value: "1" } }
                            on_enter { branch { if: "$session.tier = gold" } }
                            routes { condition: "$user.vip" target: "a" } } }
        )", &error), "bad expressions don't compile");
        check(contains(error, "session param \"count\": default doesn't fit its type NUMBER"), "default type checked");
        check(contains(error, "on_enter step 1: $session.nmae: no session param \"nmae\" is declared"), "template reference checked");
        check(contains(error, "step 2: set key \"$input.text\" must be $session.<name>, $page.locals.<name> or $form.<slot>"), "set key checked");
        check(contains(error, "step 3 branch if: \"$session.tier = gold\": column 17: unknown word 'gold'"), "parse error located");
        check(contains(error, "route 1 condition: unknown reference $user.vip"), "unknown root");

        auto stateful = compile(R"agent(
            start_flow: "main"
            session_params { name: "tier"   type: STRING default { string_value: "standard" } }
            session_params { name: "visits" type: NUMBER default { number_value: 0 } }
            intents { name: "go" examples: ["go"] }
            intents { name: "any" examples: ["any"] }
            flows { name: "main" start_page: "a"
                    pages { name: "a"
                            on_enter { set { key: "$session.visits" value: "$session.visits" } }
                            on_enter { set { key: "$page.locals.note" value: "'local'" } }
                            on_enter { switch { on: "$session.tier"
                                                cases { value: "gold" steps { say { text: "Gold." } } }
                                                default { say { text: "Hello, $session.tier." } } } }
                            routes { intent: "go" condition: "$context.open = true" steps { say { text: "Open: $page.locals.note." } } target: "b" }
                            routes { intent: "go" steps { say { text: "Closed." } } }
                            routes { condition: "$input.text : 'gold'" steps { set { key: "$session.tier" value: "'gold'" } } target: "CURRENT_PAGE" } }
                    pages { name: "b"
                            on_enter { branch { if: "exists($page.locals.note)" then { say { text: "kept" } } else { say { text: "Locals cleared." } } } }
                            on_enter { branch { if: "$nlu.intent = 'go'" then { goto { target: "a" } } } }
                            on_enter { say { text: "never said" } } } }
        )agent", &error);
        check(stateful != nullptr, "the stateful agent compiles");
        if (stateful) {
            flow::FlowRuntime conversation(stateful);
            bool open = false;
            conversation.setContextSource([&open] {
                google::protobuf::Struct context;
                (*context.mutable_fields())["open"] = flow::boolValue(open);
                return context;
            });
            check(conversation.start().text == "Hello, standard.", "switch default with a template");
            check(conversation.handleText("go").text == "Closed.", "a route whose condition fails is skipped");
            open = true;
            auto reply = conversation.handleText("go");
            check(reply.text == "Open: local. Locals cleared. Hello, standard." && conversation.pageName() == "a",
                  "condition from $context; locals cleared on a new page; branch with a goto");
            reply = conversation.handleText("make me gold please");
            check(!reply.intent.size() && reply.text == "Gold." && flow::toText(conversation.sessionParam("tier")) == "gold",
                  "a condition-only route when no intent matched, and set");
            check(!conversation.setSessionParam("visits", flow::stringValue("many"), &error) &&
                      contains(error, "doesn't fit session param \"visits\" (NUMBER)"), "a wrong type is refused");
            check(conversation.setSessionParam("visits", flow::stringValue("3")) &&
                      conversation.sessionParam("visits").number_value() == 3, "a numeric string fits NUMBER");
            check(!conversation.setSessionParam("nope", flow::stringValue("x"), &error), "undeclared params are refused");
        }
    }

    // Test cases report what differs.
    {
        auto result = flow::runFlowTest(agent, parse<vasdk::flow::FlowTestCase>(R"(
            name: "mismatch"
            turns { expect { page: "welcome" said_contains: "book" } }
            turns { user: "book" expect { page: "welcome" ends_session: true } }
        )"));
        check(result.failures.size() == 2 &&
                  contains(result.failures[0], "turn 2 \"book\": page is \"confirm\", expected \"welcome\"") &&
                  contains(result.failures[1], "ends_session is false"), "failures say what and where");
    }

    // DialogProcessor: greets on the first IDLE → LISTENING, answers turns, undoes a resumed one.
    {
        auto dialog = std::make_shared<flow::DialogProcessor>(agent);
        auto recorder = std::make_shared<Recorder>();
        std::vector<std::string> pages;
        dialog->setReplyObserver([&pages](const flow::Reply& reply, const flow::FlowRuntime&) {
            for (const auto& change : reply.pageChanges) pages.push_back(change.toPage);
        });
        dialog->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        dialog->initialize();
        dialog->start();
        auto listening = std::make_shared<transport::AgentStateFrame>();
        listening->previousState = transport::AgentState::IDLE;
        listening->currentState = transport::AgentState::LISTENING;
        dialog->processFrame(listening, pipeline::FlowDirection::DOWNSTREAM);
        dialog->processFrame(listening, pipeline::FlowDirection::DOWNSTREAM);   // greets once
        dialog->processFrame(userTurn("book"), pipeline::FlowDirection::DOWNSTREAM);
        dialog->processFrame(userTurn("book cancel my appointment", true), pipeline::FlowDirection::DOWNSTREAM);
        const auto texts = recorder->settled();
        check(texts.size() == 3 && texts[0].text == "Book or cancel?" && texts[0].isExpecting, "greeting spoken once");
        check(texts.size() == 3 && texts[1].text == "Booking. Confirm?", "turn answered");
        check(texts.size() == 3 && texts[2].text == "Cancelled.", "resumed turn routed from where the first began");
        check(pages == std::vector<std::string>({"welcome", "booking", "confirm"}), "observer sees page changes");
        check(recorder->turns == 0, "answered turns don't go on");
        dialog->stop();
        recorder->stop();
    }
    {
        // No-input: silence once a question has played raises no-input; speech cancels the wait.
        auto silentAgent = compile(R"agent(
            start_flow: "main"
            envelopes: ["standard"]
            envelope_defs { name: "standard" handlers { event: "no-input" steps { say { text: "Still there?" } } }
                            settings { no_input_ms: 150 } }
            flows { name: "main" start_page: "welcome"
                    pages { name: "welcome" on_enter { ask { say { text: "How can I help?" } } } } }
        )agent");
        check(silentAgent != nullptr, "the no-input agent compiles");
        auto dialog = std::make_shared<flow::DialogProcessor>(silentAgent);
        auto recorder = std::make_shared<Recorder>();
        dialog->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        dialog->initialize();
        dialog->start();
        auto state = [&](transport::AgentState previous, transport::AgentState current, transport::AgentStateTransition why) {
            auto frame = std::make_shared<transport::AgentStateFrame>();
            frame->previousState = previous;
            frame->currentState = current;
            frame->transition = why;
            dialog->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
        };
        using transport::AgentState;
        using transport::AgentStateTransition;
        state(AgentState::IDLE, AgentState::LISTENING, AgentStateTransition::PIPELINE_START);   // greets
        state(AgentState::SPEAKING, AgentState::LISTENING, AgentStateTransition::SYNTHESIS_COMPLETED);
        std::this_thread::sleep_for(300ms);
        auto texts = recorder->settled();
        check(texts.size() == 2 && texts[1].text == "Still there? How can I help?" && texts[1].isExpecting,
              "silence after the question played: no-input");
        state(AgentState::SPEAKING, AgentState::LISTENING, AgentStateTransition::SYNTHESIS_COMPLETED);
        state(AgentState::LISTENING, AgentState::LISTENING, AgentStateTransition::USER_STARTED_SPEAKING);
        std::this_thread::sleep_for(300ms);
        check(recorder->settled().size() == 2, "the user starting to speak cancels it");
        dialog->stop();
        recorder->stop();
    }
    {
        // Nobody connected yet (a browser peer, a call): the greeting waits for them; when they
        // leave, the next one gets a new conversation and its own greeting.
        auto dialog = std::make_shared<flow::DialogProcessor>(agent);
        auto recorder = std::make_shared<Recorder>();
        dialog->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        dialog->initialize();
        dialog->start();
        dialog->setConnected(false);
        std::this_thread::sleep_for(30ms);   // setConnected is handled on the dialog's thread
        auto listening = std::make_shared<transport::AgentStateFrame>();
        listening->previousState = transport::AgentState::IDLE;
        listening->currentState = transport::AgentState::LISTENING;
        dialog->processFrame(listening, pipeline::FlowDirection::DOWNSTREAM);
        check(recorder->settled().empty(), "no greeting while nobody is connected");
        dialog->setConnected(true);
        check(recorder->settled().size() == 1, "greeting when someone connects");
        dialog->processFrame(userTurn("book"), pipeline::FlowDirection::DOWNSTREAM);
        dialog->setConnected(false);
        dialog->setConnected(true);
        const auto texts = recorder->settled();
        check(texts.size() == 3 && texts[2].text == "Book or cancel?", "the next caller starts over with a greeting");
        dialog->stop();
        recorder->stop();
    }
    {
        auto dialog = std::make_shared<flow::DialogProcessor>(agent);
        auto recorder = std::make_shared<Recorder>();
        dialog->setPassUnmatchedTurns(true);
        dialog->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        dialog->initialize();
        dialog->start();
        dialog->processFrame(userTurn("what's the weather"), pipeline::FlowDirection::DOWNSTREAM);
        check(recorder->settled().empty() && recorder->turns == 1, "with an LLM, a turn not understood goes on to it");
        dialog->stop();
        recorder->stop();
    }

    // A session with an agent and no LLM: greets at start (always on), answers typed text, reports pages.
    {
        EventRecorder events;   // outlives the engine, whose event thread calls it
        auto tts = std::make_shared<fakes::FakeTts>();
        VAEngine::Options options;
        options.transport = std::make_shared<fakes::FakeTransport>();
        options.agent = agent;
        std::string error;
        auto engine = VAEngine::create(vasdk::engine::EngineConfig{}, fakes::makeRegistry(tts), options, &error);
        if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        engine->addListener(events.listener());
        check(contains(engine->describe(), "TurnAssembler → UserTextInput → Dialog → SpeechInput"),
              "dialog sits after turn assembly, with no LLM after it");
        check(engine->start(&error), "engine with an agent starts");
        check(events.waitFor([](const auto& event) {
            return event.has_agent_reply() && event.agent_reply().text() == "Book or cancel?";
        }), "greeting spoken at start");
        engine->injectUserText("cancel");
        check(events.waitFor([](const auto& event) {
            return event.has_agent_reply() && event.agent_reply().text() == "Cancelled.";
        }), "typed text answered by the agent");
        check(events.waitFor([](const auto& event) {
            return event.has_session_ended() && event.session_ended().page() == "welcome";
        }), "session_ended reported");
        check(events.waitFor([](const auto& event) {
            return event.has_page_changed() && event.page_changed().to_page() == "welcome" &&
                   event.page_changed().reason() == "start";
        }), "page_changed reported");
        engine->stop();
    }

    // A session's app data: $context from a ContextProvider, $session from setSessionParam.
    {
        auto valuesAgent = compile(R"(
            start_flow: "main"
            session_params { name: "tier" type: STRING default { string_value: "standard" } }
            intents { name: "status" examples: ["status"] }
            flows { name: "main" start_page: "a"
                    pages { name: "a"
                            on_enter { say { text: "Hi." } }
                            routes { intent: "status" steps { say { text: "$session.tier, battery $context.system.battery." } } } } }
        )");
        EventRecorder events;
        auto context = std::make_shared<va::context::ContextProvider>();
        context->setValue("system", nlohmann::json{{"battery", 15}});
        VAEngine::Options options;
        options.transport = std::make_shared<fakes::FakeTransport>();
        options.agent = valuesAgent;
        options.context = context;
        std::string error;
        auto engine = VAEngine::create(vasdk::engine::EngineConfig{}, fakes::makeRegistry(std::make_shared<fakes::FakeTts>()),
                                       options, &error);
        if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        engine->addListener(events.listener());
        check(engine->start(&error), "engine with app data starts");
        engine->setSessionParam("tier", flow::stringValue("gold"));
        engine->injectUserText("status");
        check(events.waitFor([](const auto& event) {
            return event.has_agent_reply() && event.agent_reply().text() == "gold, battery 15.";
        }), "the reply reads the app's session param and context");
        engine->stop();
    }

    if (failures == 0) std::printf("dialog_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
