// Context-based barge-in through the real chain a session builds: AgentStateProcessor asking an
// LlmBargeInClassifier (a fake LLM behind it), then TurnAssemblerProcessor, which makes the turns
// the LLM answers. A confirmation over the reply never becomes a turn; a question does; while the
// agent thinks, an acknowledgement keeps the reply coming and more words join the request.

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/TurnAssemblerProcessor.h>
#include <VASDK/llm/LlmBargeInClassifier.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace va;
using Transition = transport::AgentStateTransition;

namespace va::transport {
void PrintTo(AgentStateTransition transition, std::ostream* out) { *out << toString(transition); }
} // namespace va::transport

namespace {

// The LLM the barge-in decisions are asked of: one fixed answer; keeps what it was told.
class FakeCompletion : public llm::Completion {
public:
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override {
        return completeWithin(messages, 0);
    }
    std::optional<std::string> completeWithin(const std::vector<llm::ConversationMessage>& messages, int) const override {
        std::lock_guard<std::mutex> lock(_mutex);
        _told.push_back(messages.back().content);
        return _answer;
    }
    void answer(const std::string& answer) { std::lock_guard<std::mutex> lock(_mutex); _answer = answer; }
    std::vector<std::string> told() const { std::lock_guard<std::mutex> lock(_mutex); return _told; }

private:
    mutable std::mutex _mutex;
    std::string _answer = "INTERRUPT";
    mutable std::vector<std::string> _told;
};

// The end of the chain: the turns the LLM would answer, the transitions, the back-channels recorded.
class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto turn = std::dynamic_pointer_cast<transport::UserTurnFrame>(frame)) _turns.push_back(turn->text);
        else if (auto state = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame)) _transitions.push_back(state->transition);
        else if (auto heard = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame); heard && heard->isBackchannel)
            _backchannels.push_back(heard->transcriptionResult.text);
    }
    std::vector<std::string> turns() { std::lock_guard<std::mutex> lock(_mutex); return _turns; }
    std::vector<std::string> backchannels() { std::lock_guard<std::mutex> lock(_mutex); return _backchannels; }
    bool saw(Transition transition) {
        std::lock_guard<std::mutex> lock(_mutex);
        return std::find(_transitions.begin(), _transitions.end(), transition) != _transitions.end();
    }

private:
    std::mutex _mutex;
    std::vector<std::string> _turns, _backchannels;
    std::vector<Transition> _transitions;
};

class BargeInSessionTest : public ::testing::Test {
protected:
    // Declared before the processors, so they outlive them.
    std::shared_ptr<FakeCompletion> completion = std::make_shared<FakeCompletion>();
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();
    std::shared_ptr<pipeline::TurnAssemblerProcessor> assembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
    std::shared_ptr<pipeline::AgentStateProcessor> agentState = std::make_shared<pipeline::AgentStateProcessor>();

    void TearDown() override {
        agentState->stop();
        assembler->stop();
        recorder->stop();
    }

    // As Session wires it: the classifier over the LLM, the reply's words (masked) from TTS.
    void start(bool judgeWhileThinking) {
        pipeline::BackchannelConfig config;
        config.decisionMode = transport::BargeInDecisionMode::ModelWhenRulesUnsure;
        config.judgeWhileThinking = judgeWhileThinking;
        config.historyMode = pipeline::BackchannelHistoryMode::AppendToHistory;
        agentState->setBackchannelConfig(config);
        agentState->setInterruptTimeoutMs(5000);
        agentState->setBargeInClassifier(std::make_shared<llm::LlmBargeInClassifier>(completion, 800));
        agentState->setSpokenTextProvider([] { return std::string("Your table is booked for Tuesday at 7 pm."); });
        agentState->setMaskedReplyTextProvider([] { return std::string("Your table is booked for Tuesday at 7 pm."); });
        agentState->setDownstream(assembler);
        assembler->setDownstream(recorder);
        for (auto processor : std::vector<std::shared_ptr<pipeline::FrameProcessor>>{recorder, assembler, agentState}) {
            ASSERT_TRUE(processor->initialize());
            ASSERT_TRUE(processor->start());
        }
        // The user's request: a turn the LLM answers.
        userTurn(transport::TurnStateFrame::TurnState::START, false);
        userTurn(transport::TurnStateFrame::TurnState::END, false);
        transcript("Book a table for Tuesday");
        ASSERT_TRUE(va::test::waitUntil([&] { return recorder->turns().size() == 1; })) << "the request is a turn";
    }

    void send(std::shared_ptr<transport::Frame> frame) { agentState->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM); }
    void userTurn(transport::TurnStateFrame::TurnState state, bool overAgent = true) {
        send(std::make_shared<transport::TurnStateFrame>(transport::TurnStateFrame::TurnActor::USER, state,
            overAgent && state == transport::TurnStateFrame::TurnState::START
                ? transport::TurnStateFrame::TurnType::INTERRUPT : transport::TurnStateFrame::TurnType::NORMAL));
    }
    void systemTurn(transport::TurnStateFrame::TurnState state) {
        send(std::make_shared<transport::TurnStateFrame>(transport::TurnStateFrame::TurnActor::SYSTEM, state,
                                                         transport::TurnStateFrame::TurnType::NORMAL));
    }
    void transcript(const std::string& text) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = true;
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
    void userSays(const std::string& words) {
        userTurn(transport::TurnStateFrame::TurnState::START);
        audio(300);
        userTurn(transport::TurnStateFrame::TurnState::END);
        transcript(words);
    }
};

} // namespace

TEST_F(BargeInSessionTest, AConfirmationOverTheReplyNeverBecomesATurn) {
    start(false);
    systemTurn(transport::TurnStateFrame::TurnState::START);   // the reply plays
    completion->answer("BACKCHANNEL");
    userSays("Tuesday, right");
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->saw(Transition::BACKCHANNEL); }));
    ASSERT_TRUE(va::test::waitUntil([&] { return !recorder->backchannels().empty(); }));
    EXPECT_EQ(recorder->backchannels(), std::vector<std::string>{"Tuesday, right"}) << "recorded in the history, not answered";
    systemTurn(transport::TurnStateFrame::TurnState::END);     // the reply finishes
    EXPECT_FALSE(recorder->saw(Transition::BARGE_IN_INTERRUPT));
    EXPECT_EQ(recorder->turns().size(), 1u) << "no turn for the LLM to answer";
    const auto told = completion->told();
    ASSERT_EQ(told.size(), 1u);
    EXPECT_NE(told[0].find("Your table is booked for Tuesday at 7 pm."), std::string::npos) << "the model heard the reply";
    EXPECT_NE(told[0].find("Book a table for Tuesday"), std::string::npos) << "and the request";
}

TEST_F(BargeInSessionTest, AQuestionOverTheReplyIsAnswered) {
    assembler->setResumeMergeMs(0);   // the reply would play for seconds: not a resumption of the request
    start(false);
    systemTurn(transport::TurnStateFrame::TurnState::START);
    completion->answer("INTERRUPT");
    userSays("is it in the garden");
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->saw(Transition::BARGE_IN_INTERRUPT); }));
    systemTurn(transport::TurnStateFrame::TurnState::END);     // TTS stopped: the interrupt resolves
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->turns().size() == 2; })) << "the question is a turn";
    EXPECT_EQ(recorder->turns()[1], "is it in the garden");
}

TEST_F(BargeInSessionTest, AnAcknowledgementWhileThinkingKeepsTheReplyComing) {
    start(true);
    userSays("okay");   // a back-channel phrase: the rules keep the reply, no model asked
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->saw(Transition::BACKCHANNEL); }));
    EXPECT_FALSE(recorder->saw(Transition::USER_SPOKE_WHILE_THINKING)) << "the LLM's request isn't cancelled";
    EXPECT_EQ(recorder->turns().size(), 1u);
    EXPECT_TRUE(completion->told().empty());
}

TEST_F(BargeInSessionTest, MoreWordsWhileThinkingJoinTheRequest) {
    start(true);
    completion->answer("ADDS");
    userSays("for four people");
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->saw(Transition::USER_SPOKE_WHILE_THINKING); }))
        << "the coming reply is cancelled";
    systemTurn(transport::TurnStateFrame::TurnState::END);     // the cancelled reply's end: the interrupt resolves
    ASSERT_TRUE(va::test::waitUntil([&] { return recorder->turns().size() == 2; }));
    EXPECT_EQ(recorder->turns()[1], "Book a table for Tuesday for four people") << "answered as one request";
}
