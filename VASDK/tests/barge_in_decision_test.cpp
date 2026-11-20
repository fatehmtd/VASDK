// Barge-in decision modes in AgentStateProcessor (BackchannelConfig::decisionMode): who decides
// whether speech over the agent - or while it thinks - is a back-channel or a real interruption.
// WordRulesOnly never asks a model; ModelWhenRulesUnsure keeps an interrupt word and only-back-channel
// phrases for the rules and asks the model the rest; ModelForEveryOverlap asks it about all speech
// with words. A model that doesn't answer in time, or answers Undecided, leaves it to the rules.

#include <VASDK/AgentStateProcessor.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using Transition = transport::AgentStateTransition;
using Mode = transport::BargeInDecisionMode;
using pipeline::BackchannelVerdict;

namespace va::transport {
void PrintTo(AgentStateTransition transition, std::ostream* out) { *out << toString(transition); }
} // namespace va::transport

namespace {

// Records the transitions the agent state sends on.
class Recorder : public pipeline::FrameProcessor {
public:
    struct Seen { Transition transition; transport::AgentState state; };
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto agentFrame = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame))
            _transitions.push_back({agentFrame->transition, agentFrame->currentState});
    }
    std::vector<Seen> since(size_t start) {
        std::lock_guard<std::mutex> lock(_mutex);
        return {_transitions.begin() + static_cast<std::ptrdiff_t>(std::min(start, _transitions.size())), _transitions.end()};
    }
    size_t count() { std::lock_guard<std::mutex> lock(_mutex); return _transitions.size(); }
    bool saw(Transition transition, size_t start) {
        for (const auto& seen : since(start)) if (seen.transition == transition) return true;
        return false;
    }

private:
    std::mutex _mutex;
    std::vector<Seen> _transitions;
};

// A model that answers `verdict`, and can be held until released (a slow model).
class FakeClassifier : public pipeline::BargeInClassifier {
public:
    std::string name() const override { return "fake"; }
    BackchannelVerdict classify(const pipeline::BargeInContext& context) const override {
        std::unique_lock<std::mutex> lock(_mutex);
        _asked.push_back(context);
        _changed.notify_all();
        _changed.wait(lock, [this] { return !_held; });
        return _verdict;
    }
    void answer(BackchannelVerdict verdict) { std::lock_guard<std::mutex> lock(_mutex); _verdict = verdict; }
    void hold() { std::lock_guard<std::mutex> lock(_mutex); _held = true; }
    void release() {
        { std::lock_guard<std::mutex> lock(_mutex); _held = false; }
        _changed.notify_all();
    }
    size_t timesAsked() const { std::lock_guard<std::mutex> lock(_mutex); return _asked.size(); }
    pipeline::BargeInContext lastAsked() const { std::lock_guard<std::mutex> lock(_mutex); return _asked.back(); }

private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    mutable std::vector<pipeline::BargeInContext> _asked;
    BackchannelVerdict _verdict = BackchannelVerdict::Undecided;
    bool _held = false;
};

class BargeInDecisionTest : public ::testing::Test {
protected:
    // Declared before the agent state, so they outlive it.
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();
    std::shared_ptr<FakeClassifier> classifier = std::make_shared<FakeClassifier>();
    std::optional<Mode> playingPartMode;
    std::shared_ptr<pipeline::AgentStateProcessor> agentState = std::make_shared<pipeline::AgentStateProcessor>();
    size_t mark = 0;

    void TearDown() override {
        classifier->release();   // a held model would keep stop() waiting
        agentState->stop();
        recorder->stop();
    }

    // The user asks something; the agent thinks, then (speaking) starts its reply.
    void start(Mode mode, bool speaking = true, bool judgeWhileThinking = false, bool withClassifier = true) {
        pipeline::BackchannelConfig config;
        config.decisionMode = mode;
        config.judgeWhileThinking = judgeWhileThinking;
        agentState->setBackchannelConfig(config);
        agentState->setInterruptTimeoutMs(5000);   // a committed barge-in stays INTERRUPTED while checked
        if (withClassifier) agentState->setBargeInClassifier(classifier);
        agentState->setSpokenTextProvider([] { return std::string("I have a table for four on Tuesday at 7 pm."); });
        agentState->setPlayingPartDecisionProvider([this] { return playingPartMode; });
        agentState->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        agentState->initialize();
        agentState->start();   // always-on: LISTENING
        userTurn(transport::TurnStateFrame::TurnState::START, false);
        userTurn(transport::TurnStateFrame::TurnState::END, false);
        transcript("Book a table for Tuesday", true);
        if (speaking) systemTurn(transport::TurnStateFrame::TurnState::START);
        const size_t reached = speaking ? 4u : 3u;   // PIPELINE_START, USER_STARTED_SPEAKING, USER_STOPPED_SPEAKING (, SYNTHESIS_STARTED)
        ASSERT_TRUE(va::test::waitUntil([&] { return recorder->count() >= reached; })) << "the agent didn't reach its state";
        mark = recorder->count();
    }

    void send(std::shared_ptr<transport::Frame> frame) { agentState->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM); }
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
    void transcript(const std::string& text, bool isFinal) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = isFinal;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 0.9f;
        send(frame);
    }
    void audio(uint32_t durationMs) {   // 100 ms microphone frames: the audio clock the decisions run on
        for (uint32_t sent = 0; sent < durationMs; sent += 100) {
            auto frame = std::make_shared<transport::AudioFrame>();
            frame->sampleRate = 16000;
            frame->numSamples = 1600;
            send(frame);
        }
    }
    // The user says `words` over the agent and stops.
    void saysOverTheAgent(const std::string& words) {
        userStarts();
        audio(300);
        userStops();
        transcript(words, true);
    }
    bool appears(Transition transition) { return va::test::waitUntil([&] { return recorder->saw(transition, mark); }); }
    bool neverSees(Transition transition) {
        std::this_thread::sleep_for(150ms);   // fixed wait: checking that something does not happen
        return !recorder->saw(transition, mark);
    }
};

} // namespace

TEST_F(BargeInDecisionTest, WordRulesOnlyNeverAsksTheModel) {
    start(Mode::WordRulesOnly);
    saysOverTheAgent("sorry, which day?");
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT)) << "two unknown words interrupt, as before";
    EXPECT_EQ(classifier->timesAsked(), 0u);
}

TEST_F(BargeInDecisionTest, WhenUnsureAnInterruptWordCommitsWithoutTheModel) {
    start(Mode::ModelWhenRulesUnsure);
    userStarts();
    transcript("wait", false);
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT));
    EXPECT_EQ(classifier->timesAsked(), 0u);
}

TEST_F(BargeInDecisionTest, WhenUnsureABackchannelPhraseIsDismissedWithoutTheModel) {
    start(Mode::ModelWhenRulesUnsure);
    saysOverTheAgent("mm-hmm");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
    EXPECT_EQ(classifier->timesAsked(), 0u);
}

TEST_F(BargeInDecisionTest, WhenUnsureTheModelKeepsAConfirmationFromCuttingTheAgentOff) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->answer(BackchannelVerdict::Backchannel);
    saysOverTheAgent("Tuesday, right");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL)) << "the word rules alone would interrupt on \"Tuesday\"";
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT));
    EXPECT_EQ(classifier->timesAsked(), 1u);
    const auto asked = classifier->lastAsked();
    EXPECT_EQ(asked.heard, "Tuesday, right");
    EXPECT_FALSE(asked.agentIsThinking);
    EXPECT_EQ(asked.agentReply, "I have a table for four on Tuesday at 7 pm.");
    EXPECT_EQ(asked.lastUserTurn, "Book a table for Tuesday");
}

TEST_F(BargeInDecisionTest, WhenUnsureTheModelCommitsAQuestion) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->answer(BackchannelVerdict::Interrupt);
    saysOverTheAgent("is it in the garden");   // no word of the lists: only the model knows
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT));
    EXPECT_GE(classifier->timesAsked(), 1u);
}

TEST_F(BargeInDecisionTest, ForEveryOverlapTheModelJudgesInterruptWordsToo) {
    start(Mode::ModelForEveryOverlap);
    classifier->answer(BackchannelVerdict::Backchannel);
    saysOverTheAgent("no no, that's right");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL)) << "\"no\" is an interrupt word, but the model hears agreement";
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT));
}

TEST_F(BargeInDecisionTest, LongSpeechWaitsForTheModelInsteadOfTheDurationCutOff) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->hold();
    classifier->answer(BackchannelVerdict::Backchannel);
    userStarts();
    transcript("yeah yeah I see that sounds great", false);
    audio(1000);   // past commitAfterPendingMs (600), within the model's 800 ms more
    ASSERT_TRUE(va::test::waitUntil([&] { return classifier->timesAsked() >= 1; }));
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT)) << "the duration cut-off waits for the model";
    classifier->release();
    ASSERT_TRUE(va::test::waitUntil([&] { return classifier->timesAsked() >= 1; }));
    std::this_thread::sleep_for(100ms);   // fixed wait: the released verdict reaches the agent state
    audio(1000);   // the model called these words a back-channel: no cut-off for them
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT));
    userStops();
    transcript("yeah yeah I see that sounds great", true);
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
}

TEST_F(BargeInDecisionTest, WordsOnlyAtTheEndWaitForTheModelInsteadOfTheDurationCutOff) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->answer(BackchannelVerdict::Backchannel);
    userStarts();
    audio(1000);   // no partials (Whistle writes once the user stops): past commitAfterPendingMs (600), within 800 more
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT)) << "the duration cut-off waits for the words";
    userStops();
    transcript("Tuesday, right", true);
    EXPECT_TRUE(appears(Transition::BACKCHANNEL)) << "the model judged the final";
    EXPECT_EQ(classifier->lastAsked().heard, "Tuesday, right");
}

TEST_F(BargeInDecisionTest, LateWordsGiveTheModelItsWholeTime) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->hold();
    userStarts();
    audio(1000);   // no words yet, past commitAfterPendingMs (600)
    transcript("is it in the garden", false);   // the model is asked at 1000 ms
    ASSERT_TRUE(va::test::waitUntil([&] { return classifier->timesAsked() >= 1; }));
    audio(600);    // 1600 ms: past 600 + 800 counted from the cut-off, within 800 counted from the question
    EXPECT_TRUE(neverSees(Transition::BARGE_IN_INTERRUPT)) << "the model's 800 ms run from when it was asked";
    audio(300);    // 1900 ms: its time is up
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT));
}

TEST_F(BargeInDecisionTest, LongSpeechWithoutWordsStillCommits) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->answer(BackchannelVerdict::Backchannel);
    userStarts();
    audio(1500);   // past commitAfterPendingMs + modelTimeoutMs (600 + 800), and still no words
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT));
    EXPECT_EQ(classifier->timesAsked(), 0u) << "nothing to ask about";
}

TEST_F(BargeInDecisionTest, AModelTooSlowLeavesItToTheRules) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->hold();
    classifier->answer(BackchannelVerdict::Backchannel);
    userStarts();
    transcript("let me think about it", false);
    ASSERT_TRUE(va::test::waitUntil([&] { return classifier->timesAsked() >= 1; }));
    audio(1500);   // past commitAfterPendingMs + modelTimeoutMs (600 + 800)
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT)) << "the rules decide once the model's time is up";
    const size_t committed = recorder->count();
    classifier->release();   // its late answer is about speech already decided
    EXPECT_TRUE(neverSees(Transition::BACKCHANNEL));
    EXPECT_EQ(recorder->count(), committed) << "a stale verdict changes nothing";
}

TEST_F(BargeInDecisionTest, AnUndecidedModelLeavesItToTheRules) {
    start(Mode::ModelWhenRulesUnsure);
    classifier->answer(BackchannelVerdict::Undecided);
    saysOverTheAgent("Tuesday, right");
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT)) << "the rules: an unknown word in a final interrupts";
}

TEST_F(BargeInDecisionTest, WithoutAClassifierTheRulesDecide) {
    start(Mode::ModelForEveryOverlap, true, false, false);
    saysOverTheAgent("mm-hmm");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
}

TEST_F(BargeInDecisionTest, ThePlayingPartChoosesTheMode) {
    playingPartMode = Mode::ModelForEveryOverlap;   // the config says WordRulesOnly
    start(Mode::WordRulesOnly);
    classifier->answer(BackchannelVerdict::Backchannel);
    saysOverTheAgent("wait, yes");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
    EXPECT_EQ(classifier->timesAsked(), 1u) << "the part's choice wins over the config";
}

TEST_F(BargeInDecisionTest, APrivateAnswerNeverReachesTheModel) {
    start(Mode::ModelForEveryOverlap);
    auto policy = std::make_shared<transport::TurnPolicyFrame>();
    policy->privateAnswer = true;   // the agent asked for a card number
    agentState->processFrame(policy, pipeline::FlowDirection::UPSTREAM);
    classifier->answer(BackchannelVerdict::Backchannel);
    saysOverTheAgent("four two four two");
    EXPECT_TRUE(appears(Transition::BARGE_IN_INTERRUPT)) << "the rules decide";
    EXPECT_EQ(classifier->timesAsked(), 0u);
}

TEST_F(BargeInDecisionTest, WhileThinkingSpeechCancelsAtOnceByDefault) {
    start(Mode::ModelWhenRulesUnsure, false);
    userStarts();
    EXPECT_TRUE(appears(Transition::USER_SPOKE_WHILE_THINKING));
    EXPECT_FALSE(recorder->saw(Transition::BARGE_IN_PENDING, mark));
}

TEST_F(BargeInDecisionTest, WhileThinkingAnAcknowledgementKeepsTheReply) {
    start(Mode::ModelWhenRulesUnsure, false, true);
    saysOverTheAgent("okay");
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
    EXPECT_TRUE(neverSees(Transition::USER_SPOKE_WHILE_THINKING)) << "the reply coming isn't cancelled";
    const auto seen = recorder->since(mark);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.front().transition, Transition::BARGE_IN_PENDING);
    EXPECT_EQ(seen.front().state, transport::AgentState::THINKING) << "TTS pauses the first audio on this";
    EXPECT_EQ(agentState->getState(), transport::AgentState::THINKING);
}

TEST_F(BargeInDecisionTest, WhileThinkingWordsThatAddToTheRequestCancelIt) {
    start(Mode::ModelWhenRulesUnsure, false, true);
    classifier->answer(BackchannelVerdict::Interrupt);
    saysOverTheAgent("for four people");
    EXPECT_TRUE(appears(Transition::USER_SPOKE_WHILE_THINKING));
    const auto asked = classifier->lastAsked();
    EXPECT_TRUE(asked.agentIsThinking);
    EXPECT_EQ(asked.lastUserTurn, "Book a table for Tuesday") << "the request being answered";
    EXPECT_TRUE(asked.agentReply.empty());
}

TEST_F(BargeInDecisionTest, WhileThinkingTheRulesKeepTheReplyForANoise) {
    start(Mode::WordRulesOnly, false, true);
    saysOverTheAgent("");   // a cough: speech with no words
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
    EXPECT_TRUE(neverSees(Transition::USER_SPOKE_WHILE_THINKING));
}

TEST_F(BargeInDecisionTest, AReplyStartingWhileThinkingIsDecidedAsSpeechOverIt) {
    start(Mode::ModelWhenRulesUnsure, false, true);
    classifier->hold();
    classifier->answer(BackchannelVerdict::Backchannel);
    saysOverTheAgent("okay sure thing");
    ASSERT_TRUE(va::test::waitUntil([&] { return classifier->timesAsked() >= 1; }));
    systemTurn(transport::TurnStateFrame::TurnState::START);   // the reply starts, paused
    ASSERT_TRUE(appears(Transition::SYNTHESIS_STARTED));
    classifier->release();
    EXPECT_TRUE(appears(Transition::BACKCHANNEL));
    EXPECT_EQ(agentState->getState(), transport::AgentState::SPEAKING);
}
