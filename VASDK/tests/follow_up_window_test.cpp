// Follow-up listening and activation in AgentStateProcessor: after a reply has played, the mic stays
// open for the window (no wake word needed); speech in it cancels the close; it then goes IDLE, or
// stays LISTENING when always on; 0 closes at once, -1 never; an interrupted reply keeps the mic
// open. Push-to-talk listens while held; a SCHEDULED trigger wakes the agent. The event tap turns
// it into listening_window events, and WakeWordProcessor wakes an IDLE agent only.

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/AudioRingBuffer.h>
#include <VASDK/WakeWordProcessor.h>
#include <VASDK/engine/EventTapProcessor.h>
#include "vasdk/engine/events.pb.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using State = transport::AgentState;
using Transition = transport::AgentStateTransition;

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
        if (auto state = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame)) states.push_back(*state);
        if (auto trigger = std::dynamic_pointer_cast<transport::TriggerFrame>(frame)) triggers.push_back(*trigger);
    }
    transport::AgentStateFrame last() {
        std::this_thread::sleep_for(30ms);
        std::lock_guard<std::mutex> lock(mutex);
        return states.empty() ? transport::AgentStateFrame{} : states.back();
    }
    std::mutex mutex;
    std::vector<transport::AgentStateFrame> states;
    std::vector<transport::TriggerFrame> triggers;
};

struct Rig {
    std::shared_ptr<pipeline::AgentStateProcessor> agent = std::make_shared<pipeline::AgentStateProcessor>();
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();

    Rig(bool alwaysOn, int listenMs) {
        pipeline::BackchannelConfig hardBargeIn;
        hardBargeIn.enabled = false;
        agent->setBackchannelConfig(hardBargeIn);
        agent->setAlwaysOn(alwaysOn);
        agent->setFollowUpListenMs(listenMs);
        agent->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        agent->initialize();
        agent->start();
    }
    ~Rig() {
        agent->stop();
        recorder->stop();
    }
    void send(std::shared_ptr<transport::Frame> frame) { agent->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM); }
    void turn(transport::TurnStateFrame::TurnActor actor, transport::TurnStateFrame::TurnState state,
              transport::TurnStateFrame::TurnType type = transport::TurnStateFrame::TurnType::NORMAL) {
        send(std::make_shared<transport::TurnStateFrame>(actor, state, type));
    }
    void trigger(transport::TriggerType type, const std::string& data = "") {
        auto frame = std::make_shared<transport::TriggerFrame>();
        frame->triggerType = type;
        frame->triggerData = data;
        send(frame);
    }
    void userSpeaks() {
        turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::START);
        turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::END);
    }
    // A full exchange from LISTENING: the user's turn, then the reply plays out.
    void exchange() {
        userSpeaks();
        turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::START);
        turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::END);
    }
    bool is(State state, Transition transition) {
        auto last = recorder->last();
        return last.currentState == state && last.transition == transition;
    }
};

} // namespace

int main() {
    {
        Rig rig(false, 150);
        check(rig.agent->getState() == State::IDLE, "wake-word mode starts IDLE");
        rig.trigger(transport::TriggerType::WAKE_WORD);
        rig.exchange();
        auto opened = rig.recorder->last();
        check(opened.currentState == State::LISTENING && opened.transition == Transition::SYNTHESIS_COMPLETED &&
              opened.listenWindowMs == 150, "reply played: window opens, mic stays on");
        std::this_thread::sleep_for(250ms);
        check(rig.is(State::IDLE, Transition::FOLLOW_UP_WINDOW_CLOSED), "nobody spoke: IDLE when it closes");
    }
    {
        Rig rig(false, 150);
        rig.trigger(transport::TriggerType::WAKE_WORD);
        rig.exchange();
        std::this_thread::sleep_for(50ms);
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::START);
        std::this_thread::sleep_for(250ms);
        check(rig.is(State::LISTENING, Transition::USER_STARTED_SPEAKING), "speech in the window keeps it a turn");
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::END);
        check(rig.agent->getState() == State::THINKING, "follow-up answered without the wake word");
    }
    {
        Rig rig(false, 0);
        rig.trigger(transport::TriggerType::WAKE_WORD);
        rig.exchange();
        check(rig.is(State::IDLE, Transition::SYNTHESIS_COMPLETED) && rig.recorder->last().listenWindowMs == 0,
              "0: IDLE right after the reply, as before");
    }
    {
        Rig rig(false, -1);
        rig.trigger(transport::TriggerType::WAKE_WORD);
        rig.exchange();
        std::this_thread::sleep_for(200ms);
        check(rig.is(State::LISTENING, Transition::SYNTHESIS_COMPLETED) && rig.recorder->last().listenWindowMs == -1,
              "-1: the window never closes");
    }
    {
        Rig rig(false, 150);
        rig.trigger(transport::TriggerType::WAKE_WORD);
        rig.userSpeaks();
        rig.turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::START);
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::START,
                 transport::TurnStateFrame::TurnType::INTERRUPT);
        check(rig.agent->getState() == State::INTERRUPTED, "barge-in interrupts the reply");
        rig.turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::END);
        check(rig.is(State::LISTENING, Transition::INTERRUPT_RESOLVED), "interrupted reply: mic stays open");
        std::this_thread::sleep_for(250ms);
        check(rig.agent->getState() == State::LISTENING, "window doesn't close on a user who is still talking");
    }
    {
        Rig rig(true, 100);
        rig.exchange();
        std::this_thread::sleep_for(200ms);
        check(rig.is(State::LISTENING, Transition::FOLLOW_UP_WINDOW_CLOSED), "always on: still LISTENING, close reported");
    }
    {
        Rig rig(true, 0);
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::START);
        check(rig.is(State::LISTENING, Transition::USER_STARTED_SPEAKING), "speech start reported with no window open");
        rig.turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::START);
        check(rig.agent->getState() == State::LISTENING, "speech nobody asked for waits while the user speaks");
    }
    {
        Rig rig(true, 0);
        rig.turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::START);
        check(rig.is(State::SPEAKING, Transition::SYNTHESIS_STARTED), "a greeting from LISTENING is spoken as a reply");
        rig.turn(transport::TurnStateFrame::TurnActor::SYSTEM, transport::TurnStateFrame::TurnState::END);
        check(rig.is(State::LISTENING, Transition::SYNTHESIS_COMPLETED), "and listening resumes after it");
    }
    {
        Rig rig(false, 0);
        rig.trigger(transport::TriggerType::PUSH_TO_TALK, "down");
        check(rig.is(State::LISTENING, Transition::PUSH_TO_TALK), "push-to-talk: listens while held");
        rig.trigger(transport::TriggerType::PUSH_TO_TALK, "up");
        check(rig.is(State::IDLE, Transition::PUSH_TO_TALK), "push-to-talk: released in silence, IDLE");
        rig.trigger(transport::TriggerType::PUSH_TO_TALK, "down");
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::START);
        rig.trigger(transport::TriggerType::PUSH_TO_TALK, "up");
        rig.turn(transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::END);
        check(rig.agent->getState() == State::THINKING, "push-to-talk: released mid-sentence, the turn is answered");
    }
    {
        Rig rig(false, 0);
        rig.trigger(transport::TriggerType::SCHEDULED, "reminder");
        check(rig.agent->getState() == State::LISTENING, "SCHEDULED trigger wakes the agent");
    }

    // Events: OPENED with its length, EXTENDED when the user answers, CLOSED on timeout.
    {
        std::mutex mutex;
        std::vector<vasdk::engine::ListeningWindow> windows;
        {
            auto dispatcher = std::make_shared<engine::EventDispatcher>();
            dispatcher->addListener([&](const vasdk::engine::Event& event) {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.has_listening_window()) windows.push_back(event.listening_window());
            });
            engine::EventTapProcessor tap(dispatcher);
            auto stateFrame = [&](Transition transition, int windowMs = 0) {
                auto frame = std::make_shared<transport::AgentStateFrame>();
                frame->currentState = State::LISTENING;
                frame->transition = transition;
                frame->listenWindowMs = windowMs;
                tap.processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
            };
            stateFrame(Transition::SYNTHESIS_COMPLETED, 8000);
            stateFrame(Transition::USER_STARTED_SPEAKING);
            stateFrame(Transition::SYNTHESIS_COMPLETED, 8000);
            stateFrame(Transition::FOLLOW_UP_WINDOW_CLOSED);
            stateFrame(Transition::USER_STARTED_SPEAKING);   // no window open: no event
        }
        using Window = vasdk::engine::ListeningWindow;
        check(windows.size() == 4 && windows[0].state() == Window::OPENED && windows[0].closes_in_ms() == 8000 &&
              windows[1].state() == Window::EXTENDED && windows[3].state() == Window::CLOSED &&
              windows[3].reason() == "timeout", "listening_window events");
    }

    // WakeWordProcessor: a detection wakes the agent, and only while it is IDLE.
    {
        struct FakeWakeWord : audio::WakeWordProvider {
            int calls = 0;
            bool initialize(const audio::AudioConfig&) override { return true; }
            std::optional<Detection> processAudio(const char* samples, size_t numSamples) override {
                ++calls;
                for (size_t i = 0; i < numSamples * 2; ++i) if (samples[i]) return Detection{"hey garage", 0.9f};
                return std::nullopt;
            }
        };
        auto provider = std::make_shared<FakeWakeWord>();
        auto ringBuffer = std::make_shared<AudioRingBuffer>(16000, 2);
        auto wakeWord = std::make_shared<pipeline::WakeWordProcessor>(provider);
        auto recorder = std::make_shared<Recorder>();
        bool idle = true;
        wakeWord->setRingBuffer(ringBuffer);
        wakeWord->setAgentIdle([&] { return idle; });
        wakeWord->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        auto audio = [&](char value) {
            std::vector<char> pcm(3200, value);
            auto frame = std::make_shared<transport::AudioFrame>();
            frame->numSamples = 1600;
            frame->sampleSizeInBytes = 2;
            frame->postWriteOffset = ringBuffer->write(pcm.data(), 0, 1600);
            wakeWord->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
        };
        audio(0);
        audio(1);
        idle = false;
        audio(1);
        recorder->last();
        recorder->stop();
        check(recorder->triggers.size() == 1 && recorder->triggers[0].triggerType == transport::TriggerType::WAKE_WORD &&
              recorder->triggers[0].triggerData == "hey garage", "wake word detected sends one WAKE_WORD trigger");
        check(provider->calls == 2, "not listening for the wake word while the agent is awake");
    }

    if (failures == 0) std::printf("follow_up_window_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
