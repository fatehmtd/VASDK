// Rich replies: a reply in parts (one that can't be interrupted, one in SSML spoken untagged by
// voices without it), choices offered with a question and matched when said, an app UI, lint
// errors, the TextFrames the dialog stage sends for them, and delivery feedback: a read-back the
// user cut off isn't confirmed by a plain "yes".

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/DialogProcessor.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace va::flow;
using namespace std::chrono_literals;

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
goals {
  name: "booking"
  require_confirmation: true
  confirm: "An oil change on $form.day. Shall I book it?"
  slots { name: "day" type: ENUM values: ["monday", "tuesday"] prompt: "Which day?" }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { say { text: "Welcome to Acme Garage." ui { fields { key: "card" value { string_value: "logo" } } } } }
    on_enter { say { text: "Calls are recorded." interruptible: false } }
    on_enter { ask { say { ssml: "<speak>Book a service <break time='300ms'/> or hear our hours?</speak>" }
                     choices { label: "Book" value: "book" synonyms: ["make a booking"] }
                     choices { label: "Hours" } } }
    routes { intent: "book" target: "booking" }
    routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
  }
  pages { name: "booking" form { goal: "booking" } routes { condition: "$form.confirmed" target: "done" } }
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

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame)) texts.push_back(*text);
    }
    std::vector<transport::TextFrame> settled() {
        std::this_thread::sleep_for(50ms);
        std::lock_guard<std::mutex> lock(mutex);
        return texts;
    }
    std::mutex mutex;
    std::vector<transport::TextFrame> texts;
};

std::shared_ptr<transport::AgentStateFrame> agentState(transport::AgentState previous, transport::AgentState current,
                                                       transport::AgentStateTransition transition) {
    auto frame = std::make_shared<transport::AgentStateFrame>();
    frame->previousState = previous;
    frame->currentState = current;
    frame->transition = transition;
    return frame;
}

std::shared_ptr<transport::UserTurnFrame> userTurn(const std::string& text) {
    auto turn = std::make_shared<transport::UserTurnFrame>();
    turn->text = text;
    return turn;
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

    // A reply in parts: alike parts join, a part that can't be interrupted or has SSML stands alone.
    FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.text == "Welcome to Acme Garage. Calls are recorded. Book a service or hear our hours?",
          "the whole text, SSML untagged");
    check(reply.segments.size() == 3 && reply.segments[0].interruptible && !reply.segments[1].interruptible &&
              reply.segments[1].text == "Calls are recorded." && contains(reply.segments[2].ssml, "<break") &&
              reply.segments[2].text == "Book a service or hear our hours?",
          "three parts: plain, can't be interrupted, SSML");
    check(reply.choices.size() == 2 && reply.choices[0].value == "book" && reply.choices[1].value == "Hours",
          "choices, a missing value is the label");
    check(reply.ui.fields().count("card") && reply.ui.fields().at("card").string_value() == "logo", "the app UI");

    // Choices: a synonym said as the whole answer is the value; asked again, they're offered again.
    reply = conversation.handleText("hours");
    check(reply.text == "We're open 8 to 6. Book a service or hear our hours?" && reply.choices.size() == 2,
          "the question asked again offers its choices");
    reply = conversation.handleText("Make a booking!");
    check(conversation.pageName() == "booking" && reply.text == "Which day?" && reply.choices.empty(),
          "a choice's synonym routes like its value");

    // Delivery feedback: "yes" to a read-back that was cut off reads it back again.
    reply = conversation.handleText("monday");
    check(reply.text == "An oil change on monday. Shall I book it?", "the read-back");
    conversation.replyInterrupted();
    reply = conversation.handleText("yes");
    check(conversation.pageName() == "booking" && reply.text == "An oil change on monday. Shall I book it?" &&
              conversation.lookup({"form", "confirmed"}).bool_value() == false,
          "cut off: read back again, not confirmed");
    reply = conversation.handleText("yes");
    check(conversation.pageName() == "done" && reply.endsSession, "heard in full: confirmed");

    // Lint
    check(contains(lintError("say { text: \"Calls are recorded.\" interruptible: false }", "say { interruptible: false }"),
                   "say has no text"),
          "a say needs text or SSML");
    check(contains(lintError("choices { label: \"Hours\" }", "choices { value: \"hours\" }"), "choice 2 has no label"),
          "a choice needs a label");
    check(contains(lintError("Book a service <break", "$session.nope <break"), "ssml: $session.nope"), "references in SSML are checked");

    // The dialog stage: a TextFrame per part, the choices, UI and question on the last.
    {
        auto dialog = std::make_shared<DialogProcessor>(agent);
        auto recorder = std::make_shared<Recorder>();
        dialog->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        dialog->initialize();
        dialog->start();
        auto send = [&](std::shared_ptr<transport::Frame> frame) { dialog->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM); };
        send(agentState(transport::AgentState::IDLE, transport::AgentState::LISTENING,
                        transport::AgentStateTransition::APP_COMMAND));
        auto texts = recorder->settled();
        check(texts.size() == 3 && !texts[0].isFinal && !texts[1].isFinal && texts[2].isFinal, "a frame per part");
        if (texts.size() == 3) {
            check(texts[0].interruptible && !texts[1].interruptible && texts[1].text == " Calls are recorded.",
                  "the part that can't be interrupted, spaced from the one before");
            check(contains(texts[2].ssml, "<speak>") && texts[2].isExpecting && texts[2].choices.size() == 2 &&
                      texts[2].choices[0].value == "book" && contains(texts[2].uiJson, "\"card\":\"logo\""),
                  "SSML, the question, choices and UI on the last frame");
            check(texts[0].uiJson.empty() && texts[0].choices.empty(), "nothing extra on the earlier parts");
        }

        // The read-back is cut off by the user: their "yes" isn't taken as a confirmation.
        send(userTurn("book"));
        send(userTurn("tuesday"));
        send(agentState(transport::AgentState::SPEAKING, transport::AgentState::INTERRUPTED,
                        transport::AgentStateTransition::BARGE_IN_INTERRUPT));
        send(userTurn("yes"));
        texts = recorder->settled();
        check(!texts.empty() && texts.back().text == "An oil change on tuesday. Shall I book it?",
              "an interrupted read-back is read again");
        send(agentState(transport::AgentState::SPEAKING, transport::AgentState::LISTENING,
                        transport::AgentStateTransition::SYNTHESIS_COMPLETED));
        send(userTurn("yes"));
        texts = recorder->settled();
        check(!texts.empty() && texts.back().text == "Booked.", "heard in full, then confirmed");
        dialog->stop();
        recorder->stop();
    }

    if (failures == 0) std::printf("response_envelope_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
