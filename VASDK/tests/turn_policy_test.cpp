// Listening tuned to the question: a phone number's question asks for a patient turn end, a yes/no
// one for a snappy one, and words to expect go with them. The dialog sends that up the pipeline;
// the VAD holds pauses longer (or ends sooner), the turn model's guess during a pause no longer
// ends a patient turn (the VAD's end does), and the recognizer gets the words.

#include "engine_test_fakes.h"

#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/DialogProcessor.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

using namespace va;
using namespace std::chrono_literals;
using TurnEnd = transport::TurnPolicyFrame::TurnEnd;
using Expectation = vasdk::flow::Expectation;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame);
    }
    std::vector<std::shared_ptr<transport::Frame>> settled() {
        std::this_thread::sleep_for(80ms);
        std::lock_guard<std::mutex> lock(mutex);
        return frames;
    }
    std::mutex mutex;
    std::vector<std::shared_ptr<transport::Frame>> frames;
};

// Calls the user's turn over at every pause, as a turn model sure of itself would.
class EagerTurnModel : public audio::TurnDetectionProvider {
public:
    TurnEvaluation evaluate(const std::shared_ptr<transport::Frame> frame, bool) override {
        TurnEvaluation evaluation;
        if (frame->getType() == transport::FrameType::VAD_STATE &&
            std::static_pointer_cast<transport::VADStateFrame>(frame)->state == transport::VADState::PAUSED)
            evaluation.decision = Decision::TURN_END;
        return evaluation;
    }
};

class HintedStt : public fakes::FakeStt {
public:
    void setContextHints(const std::vector<std::string>& phrases) override { hints = phrases; }
    std::vector<std::string> hints;
};

std::shared_ptr<transport::VADStateFrame> vad(transport::VADState state) {
    auto frame = std::make_shared<transport::VADStateFrame>();
    frame->state = state;
    return frame;
}

std::shared_ptr<transport::TurnPolicyFrame> policy(TurnEnd turnEnd, std::vector<std::string> hints = {}) {
    auto frame = std::make_shared<transport::TurnPolicyFrame>();
    frame->turnEnd = turnEnd;
    frame->hints = std::move(hints);
    return frame;
}

int userTurnEnds(const std::vector<std::shared_ptr<transport::Frame>>& frames) {
    return static_cast<int>(std::count_if(frames.begin(), frames.end(), [](const auto& frame) {
        auto turn = std::dynamic_pointer_cast<transport::TurnStateFrame>(frame);
        return turn && turn->actor == transport::TurnStateFrame::TurnActor::USER &&
               turn->state == transport::TurnStateFrame::TurnState::END;
    }));
}

bool holds(const google::protobuf::RepeatedPtrField<std::string>& hints, const std::string& hint) {
    return std::find(hints.begin(), hints.end(), hint) != hints.end();
}

} // namespace

int main() {
    // What each question expects of the answer's end, and the words it may hold.
    vasdk::flow::Agent definition;
    (void)google::protobuf::TextFormat::ParseFromString(R"agent(
        start_flow: "main"
        intents { name: "hours" examples: ["opening hours", "book @service"] }
        entity_types { name: "service" list { values { value: "tires" synonyms: ["tire repair", "flat tire"] } } }
        goals { name: "contact" slots { name: "phone" type: PHONE prompt: "Your number?" }
                                slots { name: "sure" type: BOOLEAN prompt: "Sure?" }
                                slots { name: "service" entity: "@service" prompt: "Which service?" } }
        flows { name: "main" start_page: "contact"
          pages { name: "contact" form { goal: "contact" } routes { intent: "hours" }
                  routes { condition: "$form.complete" target: "menu" } }
          pages { name: "menu" on_enter { ask { say { text: "Oil or tires?" } choices { label: "Oil" } choices { label: "Tires" }
                                                expect { turn_end: NORMAL hints: ["lube"] } } } } }
    )agent", &definition);
    std::string error;
    auto agent = flow::CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    flow::FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.expectation.turn_end() == Expectation::PATIENT, "a phone number: patient");
    check(holds(reply.expectation.hints(), "opening hours") && !holds(reply.expectation.hints(), "book @service"),
          "the page's intents as hints, without entities");
    reply = conversation.handleText("555 123 4567");
    check(reply.expectation.turn_end() == Expectation::SNAPPY, "a yes/no: snappy");
    reply = conversation.handleText("yes");
    check(holds(reply.expectation.hints(), "flat tire"), "the slot's synonyms as hints");
    reply = conversation.handleText("tire repair");
    check(reply.expectation.turn_end() == Expectation::NORMAL && reply.expectation.hints_size() > 0 &&
              reply.expectation.hints(0) == "lube" && holds(reply.expectation.hints(), "Tires"),
          "an explicit turn end kept; its hints, then the choices");

    // The dialog sends it up with each question.
    {
        auto dialog = std::make_shared<flow::DialogProcessor>(agent);
        auto above = std::make_shared<Recorder>();
        dialog->setUpstream(above);
        above->initialize();
        above->start();
        dialog->initialize();
        dialog->start();
        auto listening = std::make_shared<transport::AgentStateFrame>();
        listening->previousState = transport::AgentState::IDLE;
        listening->currentState = transport::AgentState::LISTENING;
        dialog->processFrame(listening, pipeline::FlowDirection::DOWNSTREAM);
        std::shared_ptr<transport::TurnPolicyFrame> sent;
        for (const auto& frame : above->settled())
            if (auto turnPolicy = std::dynamic_pointer_cast<transport::TurnPolicyFrame>(frame)) sent = turnPolicy;
        check(sent && sent->turnEnd == TurnEnd::Patient && !sent->hints.empty(), "the dialog sends the policy upstream");
        dialog->stop();
        above->stop();
    }

    // The VAD: longer pauses and end for a patient turn, a shorter end for a snappy one, then back.
    {
        pipeline::VADFrameProcessor vadProcessor(std::make_shared<fakes::FakeVad>());
        pipeline::VADConfig base;
        base.maxSpeechPauseDurationMs = 500;
        base.maxSpeechEndDurationMs = 800;
        vadProcessor.setConfig(base);
        vadProcessor.processFrame(policy(TurnEnd::Patient), pipeline::FlowDirection::UPSTREAM);
        check(vadProcessor.getConfig().maxSpeechPauseDurationMs == 1000 && vadProcessor.getConfig().maxSpeechEndDurationMs == 1600,
              "patient: pauses and end held longer");
        vadProcessor.processFrame(policy(TurnEnd::Snappy), pipeline::FlowDirection::UPSTREAM);
        check(vadProcessor.getConfig().maxSpeechEndDurationMs == 300 && vadProcessor.getConfig().maxSpeechPauseDurationMs == 500,
              "snappy: the end sooner");
        vadProcessor.processFrame(policy(TurnEnd::Normal), pipeline::FlowDirection::UPSTREAM);
        check(vadProcessor.getConfig().maxSpeechEndDurationMs == 800, "normal: as configured");
    }

    // The turn model: its guess at a pause ends a normal turn, not a patient one, whose end is the VAD's.
    auto turnEndsWith = [](std::optional<TurnEnd> turnEnd) {
        auto turn = std::make_shared<pipeline::TurnDetectionProcessor>(std::make_shared<EagerTurnModel>());
        auto below = std::make_shared<Recorder>();
        auto above = std::make_shared<Recorder>();
        turn->setDownstream(below);
        turn->setUpstream(above);
        below->initialize();
        below->start();
        above->initialize();
        above->start();
        if (turnEnd) turn->processFrame(policy(*turnEnd), pipeline::FlowDirection::UPSTREAM);
        turn->processFrame(vad(transport::VADState::STARTING), pipeline::FlowDirection::DOWNSTREAM);
        turn->processFrame(vad(transport::VADState::ONGOING), pipeline::FlowDirection::DOWNSTREAM);
        turn->processFrame(vad(transport::VADState::PAUSED), pipeline::FlowDirection::DOWNSTREAM);
        const int atPause = userTurnEnds(below->settled());
        turn->processFrame(vad(transport::VADState::ENDED), pipeline::FlowDirection::DOWNSTREAM);
        const int atEnd = userTurnEnds(below->settled());
        below->stop();
        above->stop();
        return std::make_pair(atPause, atEnd);
    };
    const auto normal = turnEndsWith(std::nullopt);
    check(normal.first == 1, "normal: the model ends the turn at the pause");
    const auto patient = turnEndsWith(TurnEnd::Patient);
    check(patient.first == 0 && patient.second == 1, "patient: not at the pause; at the VAD's end");

    // The recognizer gets the words.
    {
        auto stt = std::make_shared<HintedStt>();
        pipeline::TranscriptionFrameProcessor transcription(stt);
        transcription.processFrame(policy(TurnEnd::Normal, {"oil change", "tire repair"}), pipeline::FlowDirection::UPSTREAM);
        check(stt->hints == std::vector<std::string>({"oil change", "tire repair"}), "hints reach the recognizer");
    }

    if (failures == 0) std::printf("turn_policy_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
