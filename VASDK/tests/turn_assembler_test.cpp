// TurnAssemblerProcessor: one UserTurnFrame per user turn, whichever of the final transcription and
// the turn end comes first; the watchdog and the fire policy when the final is late or empty; a
// turn sent on its partial drops the late final; resumed turns are merged (replacing the previous
// one only when it was already sent, and only within the window); back-channels are not turns.

#include <VASDK/TurnAssemblerProcessor.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;

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
        if (auto turn = std::dynamic_pointer_cast<transport::UserTurnFrame>(frame)) turns.push_back(*turn);
        if (auto t = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame); t && t->isFollowUp)
            followUps.push_back(t->transcriptionResult.text);
    }
    std::vector<transport::UserTurnFrame> settledTurns() {
        std::this_thread::sleep_for(60ms);
        std::lock_guard<std::mutex> lock(mutex);
        return turns;
    }
    std::mutex mutex;
    std::vector<transport::UserTurnFrame> turns;
    std::vector<std::string> followUps;
};

struct Rig {
    std::shared_ptr<pipeline::TurnAssemblerProcessor> assembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();

    explicit Rig(std::shared_ptr<llm::LLMFirePolicyInterface> policy = nullptr, uint32_t watchdogMs = 2000) {
        assembler->setFirePolicy(policy);
        assembler->setFinalWatchdogTimeoutMs(watchdogMs);
        assembler->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        assembler->initialize();
        assembler->start();
    }
    ~Rig() {
        assembler->stop();
        recorder->stop();
    }
    void send(std::shared_ptr<transport::Frame> frame) {
        assembler->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    }
    void agent(transport::AgentStateTransition transition) {
        auto frame = std::make_shared<transport::AgentStateFrame>();
        frame->transition = transition;
        send(frame);
    }
    void stopped() { agent(transport::AgentStateTransition::USER_STOPPED_SPEAKING); }
    void transcript(const std::string& text, bool isFinal, bool isBackchannel = false) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = isFinal;
        frame->isBackchannel = isBackchannel;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 0.9f;
        send(frame);
    }
    void turnStateEnd() {
        send(std::make_shared<transport::TurnStateFrame>(transport::TurnStateFrame::TurnActor::USER,
                                                         transport::TurnStateFrame::TurnState::END,
                                                         transport::TurnStateFrame::TurnType::NORMAL));
    }
};

std::vector<std::string> texts(const std::vector<transport::UserTurnFrame>& turns) {
    std::vector<std::string> out;
    for (const auto& turn : turns) out.push_back(turn.text);
    return out;
}

using Texts = std::vector<std::string>;
auto finalOnly() { return std::make_shared<llm::FinalOnlyFirePolicy>(); }

} // namespace

int main() {
    {
        Rig rig;
        rig.stopped();
        rig.transcript("Where is my order?", true);
        check(texts(rig.recorder->settledTurns()) == Texts{"Where is my order?"}, "turn end, then final");
    }
    {
        Rig rig;
        rig.transcript("Where is my order?", true);
        rig.stopped();
        rig.transcript("Second", true);
        rig.stopped();
        rig.transcript("Third", true);
        rig.turnStateEnd();
        const auto turns = rig.recorder->settledTurns();
        check(texts(turns) == Texts{"Where is my order?", "Second", "Third"}, "staged final sent at the turn end");
        check(turns.size() == 3 && turns[0].turnId < turns[1].turnId, "turn ids increase");
    }
    {
        Rig rig;
        rig.transcript("Book it", true);
        rig.turnStateEnd();
        check(texts(rig.recorder->settledTurns()) == Texts{"Book it"}, "staged final sent on TurnStateFrame END");
    }
    {
        Rig eager;
        eager.transcript("cancel my", false);
        eager.stopped();
        eager.transcript("", true);
        check(texts(eager.recorder->settledTurns()) == Texts{"cancel my"}, "empty final: partial sent (eager)");
        Rig strict(finalOnly());
        strict.transcript("cancel my", false);
        strict.stopped();
        strict.transcript("", true);
        check(strict.recorder->settledTurns().empty(), "empty final: partial kept back (final only)");
    }
    {
        Rig rig(nullptr, 100);
        rig.transcript("cancel my order", false);
        rig.stopped();
        std::this_thread::sleep_for(250ms);
        check(texts(rig.recorder->settledTurns()) == Texts{"cancel my order"}, "watchdog sends the partial");
        Rig staged(finalOnly(), 100);
        staged.transcript("Book a table", true);
        std::this_thread::sleep_for(250ms);
        check(texts(staged.recorder->settledTurns()) == Texts{"Book a table"}, "watchdog sends a staged final");
    }
    {
        Rig rig(nullptr, 100);
        rig.transcript("What time is it", false);
        rig.agent(transport::AgentStateTransition::TRANSCRIPTION_READY);
        rig.turnStateEnd();
        rig.transcript("What time is it?", true);
        std::this_thread::sleep_for(250ms);
        check(texts(rig.recorder->settledTurns()) == Texts{"What time is it"}, "sent on the partial, late final dropped");
    }
    for (const bool enabled : {true, false}) {
        Rig rig;
        if (!enabled) rig.assembler->setResumeMergeMs(0);
        rig.stopped();
        rig.transcript("Book a flight", true);
        rig.agent(transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING);
        rig.stopped();
        rig.transcript("to Paris", true);
        const auto turns = rig.recorder->settledTurns();
        const std::string second = enabled ? "Book a flight to Paris" : "to Paris";
        check(texts(turns) == Texts{"Book a flight", second},
              enabled ? "[resume] merged with the sent turn" : "[resume off] not merged");
        check(turns.size() == 2 && turns[1].replacesPrevious == enabled, "[resume] replaces the sent turn");
        std::lock_guard<std::mutex> lock(rig.recorder->mutex);
        check(rig.recorder->followUps == (enabled ? Texts{second} : Texts{}), "[resume] merged text announced");
    }
    {
        Rig rig;
        rig.assembler->setResumeMergeMs(50);
        rig.stopped();
        rig.transcript("Book a flight", true);
        std::this_thread::sleep_for(100ms);
        rig.agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT);
        rig.stopped();
        rig.transcript("Stop", true);
        check(texts(rig.recorder->settledTurns()) == Texts{"Book a flight", "Stop"}, "barge-in after the window: new turn");
    }
    {
        Rig rig;
        rig.transcript("Book a flight to", true);   // staged, never sent: the turn end judged it unfinished
        rig.agent(transport::AgentStateTransition::USER_STARTED_SPEAKING);
        rig.stopped();
        rig.transcript("Paris.", true);
        const auto turns = rig.recorder->settledTurns();
        check(texts(turns) == Texts{"Book a flight to Paris."}, "[unsent] merged into the next turn");
        check(turns.size() == 1 && !turns[0].replacesPrevious, "[unsent] nothing to replace");
    }
    {
        Rig rig;
        rig.transcript("mm-hmm", true, true);
        rig.agent(transport::AgentStateTransition::BACKCHANNEL);
        rig.stopped();
        rig.transcript("What about Tuesday?", true);
        check(texts(rig.recorder->settledTurns()) == Texts{"What about Tuesday?"}, "back-channel is not a turn");
    }

    if (failures == 0) std::printf("turn_assembler_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
