// Back-channel handling in AgentStateProcessor: user speech while the agent is SPEAKING is held as
// a pending barge-in, then dismissed as a back-channel ("mm-hmm": the agent keeps talking, the LLM
// never answers it) or committed as a real interruption (BARGE_IN_INTERRUPT, and a turn that ended
// unseen is replayed to the LLM once the interrupt resolves). Speech over the end of a reply is the
// user's turn, and so is speech over a part of the reply that can't be interrupted. Also covers the classifier and PlaybackHoldEnvelope (duck / pause ramps).

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/PlaybackHoldEnvelope.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using Transition = transport::AgentStateTransition;

namespace {

int failures = 0;
void check(bool ok, const char* scenario, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL [%s] %s\n", scenario, what); }
}

struct ForwardedTranscription {
    std::string text;
    bool isFinal;
    bool isBackchannel;
};

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto agentFrame = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame)) {
            transitions.push_back(agentFrame->transition);
        } else if (auto transcription = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame)) {
            transcriptions.push_back({transcription->transcriptionResult.text, transcription->isFinal,
                                      transcription->isBackchannel});
        }
    }
    std::vector<Transition> transitionsSince(size_t start) {
        std::this_thread::sleep_for(60ms);  // let the recorder's queue drain
        std::lock_guard<std::mutex> lock(mutex);
        return std::vector<Transition>(transitions.begin() + static_cast<std::ptrdiff_t>(std::min(start, transitions.size())),
                                       transitions.end());
    }
    std::vector<ForwardedTranscription> finals() {
        std::this_thread::sleep_for(60ms);
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ForwardedTranscription> result;
        for (auto& transcription : transcriptions) if (transcription.isFinal) result.push_back(transcription);
        return result;
    }
    size_t transitionCount() {
        std::this_thread::sleep_for(60ms);
        std::lock_guard<std::mutex> lock(mutex);
        return transitions.size();
    }
    std::mutex mutex;
    std::vector<Transition> transitions;
    std::vector<ForwardedTranscription> transcriptions;
};

struct Rig {
    std::shared_ptr<pipeline::AgentStateProcessor> agentState = std::make_shared<pipeline::AgentStateProcessor>();
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();
    int playbackRemainingMs = -1;
    int protectedMs = 0;   // a part that can't be interrupted is playing
    size_t mark = 0;

    // interruptTimeoutMs: long by default, so a check made after a committed barge-in doesn't see the
    // timeout's resolution on a slow machine (a stalled CI runner); E9 sets a short one to test it.
    explicit Rig(pipeline::BackchannelConfig backchannelConfig = {}, int interruptTimeoutMs = 2000) {
        agentState->setBackchannelConfig(backchannelConfig);
        agentState->setInterruptTimeoutMs(interruptTimeoutMs);
        agentState->setPlaybackRemainingMsProvider([this] { return playbackRemainingMs; });
        agentState->setProtectedPlaybackMsProvider([this] { return protectedMs; });
        agentState->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        agentState->initialize();
        agentState->start();  // always-on: LISTENING
        // A normal turn, then the reply starts: SPEAKING.
        userTurn(transport::TurnStateFrame::TurnState::START, false);
        userTurn(transport::TurnStateFrame::TurnState::END, false);
        systemTurn(transport::TurnStateFrame::TurnState::START);
        mark = recorder->transitionCount();
    }
    ~Rig() { agentState->stop(); recorder->stop(); }

    void send(std::shared_ptr<transport::Frame> frame) {
        agentState->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    }
    void userTurn(transport::TurnStateFrame::TurnState state, bool overAgent = true) {
        auto frame = std::make_shared<transport::TurnStateFrame>();
        frame->actor = transport::TurnStateFrame::TurnActor::USER;
        frame->state = state;
        frame->type = overAgent && state == transport::TurnStateFrame::TurnState::START
            ? transport::TurnStateFrame::TurnType::INTERRUPT : transport::TurnStateFrame::TurnType::NORMAL;
        send(frame);
    }
    void userStarts() { userTurn(transport::TurnStateFrame::TurnState::START); }
    void userStops() { userTurn(transport::TurnStateFrame::TurnState::END); }
    void systemTurn(transport::TurnStateFrame::TurnState state) {
        auto frame = std::make_shared<transport::TurnStateFrame>();
        frame->actor = transport::TurnStateFrame::TurnActor::SYSTEM;
        frame->state = state;
        send(frame);
    }
    void replyEnds() { systemTurn(transport::TurnStateFrame::TurnState::END); }  // also INTERRUPTED -> END
    void transcript(const std::string& text, bool isFinal) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = isFinal;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 0.9f;
        send(frame);
    }
    void audio(uint32_t durationMs) {
        for (uint32_t sent = 0; sent < durationMs; sent += 100) {
            auto frame = std::make_shared<transport::AudioFrame>();
            frame->sampleRate = 16000;
            frame->numSamples = 1600;
            send(frame);
        }
    }
    std::vector<Transition> transitions() { return recorder->transitionsSince(mark); }
};

bool sameTransitions(const std::vector<Transition>& actual, const std::vector<Transition>& expected) {
    if (actual != expected) {
        std::string text;
        for (auto transition : actual) text += transport::toString(transition) + " ";
        std::fprintf(stderr, "    got: %s\n", text.c_str());
        return false;
    }
    return true;
}

// The defaults, with VASDK's built-in en-US back-channels and interruptions.
pipeline::BackchannelConfig englishConfig() {
    pipeline::BackchannelConfig config;
    pipeline::useLanguageWords(config, nlu::Languages({nlu::Language::builtIn()}));
    return config;
}

void classifierTable() {
    const pipeline::BackchannelConfig config = englishConfig();
    struct Case { const char* text; bool isFinal; pipeline::BackchannelVerdict expected; };
    const Case cases[] = {
        {"Mm-hmm.", true, pipeline::BackchannelVerdict::Backchannel},
        {"uh huh", true, pipeline::BackchannelVerdict::Backchannel},
        {"Mhmm", true, pipeline::BackchannelVerdict::Backchannel},
        {"Yeah, yeah.", true, pipeline::BackchannelVerdict::Backchannel},
        {"I see.", true, pipeline::BackchannelVerdict::Backchannel},
        {"All right.", true, pipeline::BackchannelVerdict::Backchannel},
        {"Okay, got it!", true, pipeline::BackchannelVerdict::Backchannel},
        {"", true, pipeline::BackchannelVerdict::Backchannel},
        {"okay stop", false, pipeline::BackchannelVerdict::Interrupt},
        {"Wait", false, pipeline::BackchannelVerdict::Interrupt},
        {"yeah but the price", false, pipeline::BackchannelVerdict::Interrupt},
        {"Tuesday.", true, pipeline::BackchannelVerdict::Interrupt},
        {"yeah yeah okay right sure", true, pipeline::BackchannelVerdict::Interrupt},
        {"the", false, pipeline::BackchannelVerdict::Undecided},
        {"yeah", false, pipeline::BackchannelVerdict::Undecided},
        {"", false, pipeline::BackchannelVerdict::Undecided},
    };
    // Short garbles over the agent's voice don't interrupt; interrupt words and longer speech still do.
    check(pipeline::classifyBackchannel("Hey, hey.", true, config, 0.48f) == pipeline::BackchannelVerdict::Backchannel,
          "classifier", "low-confidence 2-word final is a back-channel");
    check(pipeline::classifyBackchannel("Hey, hey.", true, config, 0.9f) == pipeline::BackchannelVerdict::Interrupt,
          "classifier", "the same words heard clearly interrupt");
    check(pipeline::classifyBackchannel("Stop.", true, config, 0.3f) == pipeline::BackchannelVerdict::Interrupt,
          "classifier", "an interrupt word interrupts at any confidence");
    check(pipeline::classifyBackchannel("What about Tuesday?", true, config, 0.4f) == pipeline::BackchannelVerdict::Interrupt,
          "classifier", "three words interrupt at any confidence");
    for (const auto& testCase : cases) {
        const auto verdict = pipeline::classifyBackchannel(testCase.text, testCase.isFinal, config);
        if (verdict != testCase.expected) {
            ++failures;
            std::fprintf(stderr, "FAIL [classifier] \"%s\" (%s) -> %d, expected %d\n", testCase.text,
                         testCase.isFinal ? "final" : "partial", static_cast<int>(verdict),
                         static_cast<int>(testCase.expected));
        }
    }
}

void backchannelDismissed() {
    const char* scenario = "F1 back-channel";
    Rig rig;
    rig.userStarts();
    rig.audio(100);
    rig.transcript("mm", false);
    rig.audio(100);
    rig.userStops();
    rig.audio(100);
    rig.transcript("Mm-hmm.", true);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
          "pending, then dismissed");
    check(rig.agentState->getState() == transport::AgentState::SPEAKING, scenario, "agent keeps speaking");
    check(rig.recorder->finals().empty(), scenario, "the back-channel final never reaches the LLM");
}

void backchannelAppendedToHistory() {
    const char* scenario = "F6 append";
    pipeline::BackchannelConfig config;
    config.historyMode = pipeline::BackchannelHistoryMode::AppendToHistory;
    Rig rig(config);
    rig.userStarts();
    rig.userStops();
    rig.transcript("Right.", true);
    const auto finals = rig.recorder->finals();
    check(finals.size() == 1 && finals.back().isBackchannel && finals.back().text == "Right.", scenario,
          "forwarded once, marked isBackchannel");
}

void interruptWordCommits() {
    const char* scenario = "F2 interrupt word";
    Rig rig;
    rig.userStarts();
    rig.audio(100);
    rig.transcript("wait", false);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT}), scenario,
          "commits on the partial");
    rig.replyEnds();  // TTS confirms the interrupt
    rig.userStops();
    rig.transcript("Wait, how much is it?", true);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT,
                                              Transition::INTERRUPT_RESOLVED, Transition::USER_STOPPED_SPEAKING}),
          scenario, "the rest is an ordinary user turn");
    check(rig.recorder->finals().size() == 1, scenario, "its final reaches the LLM");
}

void longSpeechCommits() {
    const char* scenario = "F3 duration";
    Rig rig;
    rig.userStarts();
    rig.audio(600);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING}), scenario, "600 ms is still pending");
    rig.audio(100);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT}), scenario,
          "commits past commitAfterPendingMs");
}

void lateCommitReplaysTurn() {
    const char* scenario = "F4 late commit";
    Rig rig;
    rig.userStarts();
    rig.transcript("okay", false);
    rig.userStops();
    rig.transcript("Okay, Tuesday.", true);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT}), scenario,
          "commits on the final");
    rig.replyEnds();
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT,
                                              Transition::INTERRUPT_RESOLVED, Transition::USER_STOPPED_SPEAKING}),
          scenario, "the turn is replayed once the interrupt resolves");
    const auto finals = rig.recorder->finals();
    check(finals.size() == 1 && finals.back().text == "Okay, Tuesday." && !finals.back().isBackchannel, scenario,
          "the held final reaches the LLM exactly once");
}

void lateCommitOnTimeout() {
    const char* scenario = "E9 timeout replay";
    Rig rig({}, 150);
    rig.userStarts();
    rig.userStops();
    rig.transcript("The blue one.", true);
    std::this_thread::sleep_for(300ms);  // TTS never confirms: the interrupt timeout resolves
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT,
                                              Transition::INTERRUPT_RESOLVED, Transition::USER_STOPPED_SPEAKING}),
          scenario, "replayed from the timeout thread");
    check(rig.recorder->finals().size() == 1, scenario, "held final forwarded once");
}

void tailOverlapIsATurn() {
    const char* scenario = "F5 tail overlap";
    Rig rig;
    rig.playbackRemainingMs = 400;
    rig.userStarts();
    rig.userStops();
    rig.transcript("Yes.", true);
    check(sameTransitions(rig.transitions(), {}), scenario, "no pending, the reply finishes");
    rig.replyEnds();
    check(sameTransitions(rig.transitions(), {Transition::SYNTHESIS_COMPLETED, Transition::USER_STOPPED_SPEAKING}),
          scenario, "answered after the reply");
    const auto finals = rig.recorder->finals();
    check(finals.size() == 1 && finals.back().text == "Yes.", scenario, "the final reaches the LLM");
}

void tailOverlapClassified() {
    const char* scenario = "E19 tail classify";
    pipeline::BackchannelConfig config;
    config.tailOverlapMode = pipeline::TailOverlapMode::Classify;
    Rig rig(config);
    rig.playbackRemainingMs = 400;
    rig.userStarts();
    rig.userStops();
    rig.transcript("Yes.", true);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
          "classified and dismissed");
}

void replyEndsBeforeFinal() {
    {
        const char* scenario = "E3 back-channel final after the reply";
        Rig rig;
        rig.userStarts();
        rig.userStops();
        rig.replyEnds();
        rig.transcript("Mm-hmm.", true);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::SYNTHESIS_COMPLETED}),
              scenario, "reply completes, back-channel not answered");
        check(rig.recorder->finals().empty(), scenario, "final dropped");
    }
    {
        const char* scenario = "E3 real final after the reply";
        Rig rig;
        rig.userStarts();
        rig.userStops();
        rig.replyEnds();
        rig.transcript("The blue one.", true);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::SYNTHESIS_COMPLETED,
                                                  Transition::USER_STOPPED_SPEAKING}),
              scenario, "answered as a turn");
        check(rig.recorder->finals().size() == 1, scenario, "final forwarded");
    }
    {
        const char* scenario = "E3 still talking as the reply ends";
        Rig rig;
        rig.userStarts();
        rig.replyEnds();
        rig.userStops();
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::SYNTHESIS_COMPLETED,
                                                  Transition::USER_STOPPED_SPEAKING}),
              scenario, "an ordinary user turn");
    }
}

void twoBackchannels() {
    const char* scenario = "E4 yeah... yeah";
    Rig rig;
    rig.userStarts();
    rig.audio(300);
    rig.userStops();
    rig.audio(100);
    rig.userStarts();
    rig.audio(300);  // 600 ms of speech in all, the gap not counted
    rig.userStops();
    rig.transcript("Yeah. Yeah.", true);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
          "one pending, one dismissal");
}

void silentOverlapDismissed() {
    {
        const char* scenario = "E7 no words";
        Rig rig;
        rig.userStarts();
        rig.userStops();
        rig.audio(400);  // finalTranscriptionWaitMs passes with nothing heard
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
              "dismissed after the wait");
    }
    {
        const char* scenario = "E8 no final, decided on the partial";
        Rig rig;
        rig.userStarts();
        rig.transcript("tuesday", false);
        rig.userStops();
        rig.audio(400);
        rig.replyEnds();
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT,
                                                  Transition::INTERRUPT_RESOLVED, Transition::USER_STOPPED_SPEAKING}),
              scenario, "late commit from the partial");
        const auto finals = rig.recorder->finals();
        check(finals.size() == 1 && finals.back().text == "tuesday", scenario, "replayed with the partial text");
    }
}

void disabledIsAHardBargeIn() {
    const char* scenario = "E12 disabled";
    pipeline::BackchannelConfig config;
    config.enabled = false;
    Rig rig(config);
    rig.userStarts();
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_INTERRUPT}), scenario, "today's barge-in");
}

void protectedPartHoldsSpeech() {
    const char* scenario = "R1 can't be interrupted";
    Rig rig;
    rig.protectedMs = 2000;
    rig.userStarts();
    rig.audio(1200);
    rig.transcript("wait", false);
    rig.userStops();
    rig.transcript("Wait, actually Wednesday.", true);
    check(sameTransitions(rig.transitions(), {}), scenario, "no barge-in: the part plays to its end");
    rig.protectedMs = 0;
    rig.replyEnds();
    check(sameTransitions(rig.transitions(), {Transition::SYNTHESIS_COMPLETED, Transition::USER_STOPPED_SPEAKING}),
          scenario, "answered after the reply");
    const auto finals = rig.recorder->finals();
    check(finals.size() == 1 && finals.back().text == "Wait, actually Wednesday.", scenario, "the final is the user's turn");
}

void protectedPartAfterPending() {
    const char* scenario = "R2 pending when the part begins";
    Rig rig;
    rig.userStarts();
    rig.audio(100);
    rig.protectedMs = 2000;
    rig.transcript("wait", false);
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
          "the commit is held and the duck released");
    rig.protectedMs = 0;
    rig.userStops();
    rig.transcript("Wait.", true);
    rig.replyEnds();
    check(rig.recorder->finals().size() == 1, scenario, "answered after the reply");
}

void protectedPartWithHardBargeIn() {
    const char* scenario = "R3 can't be interrupted, back-channel off";
    pipeline::BackchannelConfig config;
    config.enabled = false;
    Rig rig(config);
    rig.protectedMs = 2000;
    rig.userStarts();
    check(sameTransitions(rig.transitions(), {}), scenario, "no barge-in while the part plays");
    rig.protectedMs = 0;
    rig.userStops();
    rig.userStarts();
    check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_INTERRUPT}), scenario, "after it, speech interrupts");
}

void envelopeRamps() {
    const char* scenario = "envelope";
    audio::PlaybackHoldEnvelope envelope;
    envelope.setRampFrameCount(320);
    std::vector<int16_t> chunk(320, 10000);
    envelope.apply(chunk.data(), chunk.size(), 1);
    check(chunk.back() == 10000, scenario, "unity until held");

    envelope.setHold(transport::PendingPlaybackMode::Duck, true, -12.0f);
    std::fill(chunk.begin(), chunk.end(), 10000);
    envelope.apply(chunk.data(), chunk.size(), 1);
    bool monotonic = true;
    for (size_t index = 1; index < chunk.size(); ++index) monotonic &= chunk[index] <= chunk[index - 1];
    const float duckGain = std::pow(10.0f, -12.0f / 20.0f);
    check(monotonic, scenario, "duck ramps down monotonically");
    check(std::abs(chunk.back() - 10000 * duckGain) < 2.0f, scenario, "duck settles at -12 dB");
    check(!envelope.isPausedSilent(), scenario, "duck never reports paused");

    envelope.setHold(transport::PendingPlaybackMode::Duck, false, -12.0f);
    std::vector<float> floatChunk(320, 1.0f);
    envelope.apply(floatChunk.data(), floatChunk.size(), 1);
    check(std::abs(floatChunk.back() - 1.0f) < 1e-4f && envelope.isUnity(), scenario, "release returns to unity (f32)");

    envelope.setHold(transport::PendingPlaybackMode::Pause, true, -12.0f);
    std::fill(chunk.begin(), chunk.end(), 10000);
    check(!envelope.isPausedSilent(), scenario, "pause fades before reporting silent");
    envelope.apply(chunk.data(), chunk.size(), 1);
    check(chunk.back() == 0 && envelope.isPausedSilent(), scenario, "pause reaches silence and reports it");

    envelope.setHold(transport::PendingPlaybackMode::Pause, false, -12.0f);
    check(!envelope.isPausedSilent(), scenario, "release lets the caller consume again");
    std::fill(chunk.begin(), chunk.end(), 10000);
    envelope.apply(chunk.data(), chunk.size(), 1);
    check(chunk.front() < 100 && chunk.back() == 10000, scenario, "resume ramps back up from silence");

    std::vector<int16_t> stereo(640, 10000);
    envelope.setHold(transport::PendingPlaybackMode::Duck, true, -12.0f);
    envelope.apply(stereo.data(), 320, 2);
    check(stereo[638] == stereo[639], scenario, "channels of a frame share the gain");

    envelope.setHold(transport::PendingPlaybackMode::Continue, true, -12.0f);
    envelope.reset();
    check(envelope.isUnity(), scenario, "reset is unity");
}

} // namespace

// Echo from open speakers: the agent's own words heard back don't interrupt it.
void echoIsNotAnInterruption() {
    const char* scenario = "E echo";
    const pipeline::BackchannelConfig defaults = englishConfig();
    const std::string reply = "Your car will be ready at five, and the total is ninety dollars.";
    check(pipeline::isEcho("your car will be ready", reply, defaults), scenario, "its own words are echo");
    check(pipeline::isEcho("car will be ready at fire", reply, defaults), scenario, "a word misheard still is (5 of 6)");
    check(!pipeline::isEcho("can you make it six instead", reply, defaults), scenario, "other words aren't");
    check(!pipeline::isEcho("wait ready at five", reply, defaults), scenario, "an interrupt word always interrupts");
    check(!pipeline::isEcho("ready", reply, defaults), scenario, "one word isn't enough to tell");
    pipeline::BackchannelConfig off;
    off.rejectEcho = true;
    check(!pipeline::isEcho("your car", "", off), scenario, "nothing being said: no echo");

    {
        Rig rig;
        rig.agentState->setSpokenTextProvider([&reply] { return reply; });
        rig.userStarts();
        rig.transcript("your car will be ready", false);
        rig.audio(800);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING}), scenario,
              "long speech that is the reply's words: still pending");
        rig.userStops();
        rig.transcript("Your car will be ready at five.", true);
        rig.audio(400);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BACKCHANNEL}), scenario,
              "dismissed as echo: the agent keeps speaking");
        check(rig.recorder->finals().empty(), scenario, "and it isn't a user turn");
    }
    {
        Rig rig;
        rig.agentState->setSpokenTextProvider([&reply] { return reply; });
        rig.userStarts();
        rig.transcript("can you make it six", false);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT}), scenario,
              "the user's own words interrupt");
    }
    {
        pipeline::BackchannelConfig waiting;
        waiting.echoWordsWaitMs = 600;
        Rig rig(waiting);
        rig.agentState->setSpokenTextProvider([&reply] { return reply; });
        rig.userStarts();
        rig.audio(700);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING}), scenario, "no words yet: it waits for them");
        rig.audio(600);
        check(sameTransitions(rig.transitions(), {Transition::BARGE_IN_PENDING, Transition::BARGE_IN_INTERRUPT}), scenario,
              "and interrupts once echoWordsWaitMs has passed without any");
    }
}

int main() {
    classifierTable();
    echoIsNotAnInterruption();
    backchannelDismissed();
    backchannelAppendedToHistory();
    interruptWordCommits();
    longSpeechCommits();
    lateCommitReplaysTurn();
    lateCommitOnTimeout();
    tailOverlapIsATurn();
    tailOverlapClassified();
    replyEndsBeforeFinal();
    twoBackchannels();
    silentOverlapDismissed();
    disabledIsAHardBargeIn();
    protectedPartHoldsSpeech();
    protectedPartAfterPending();
    protectedPartWithHardBargeIn();
    envelopeRamps();
    std::printf("backchannel checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
