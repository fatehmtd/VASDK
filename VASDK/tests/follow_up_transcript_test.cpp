// What a UI hears when the user resumes a turn: TurnAssemblerProcessor feeding EventTapProcessor, as
// in a session. Each part's final arrives as a transcript, then one follow-up with the whole turn
// that says how many of those transcripts it replaces: the turn sent while the agent thought, words
// never sent, a turn resumed twice, a turn sent on its partial, and nothing when nothing merged.

#include <VASDK/TurnAssemblerProcessor.h>
#include <VASDK/engine/EventTapProcessor.h>
#include "vasdk/engine/events.pb.h"

#include "support/TestSupport.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace va;

namespace {

using Transition = transport::AgentStateTransition;

// A transcript as a UI shows it: "text", or "text [follow-up replacing N]".
std::string shown(const vasdk::engine::Transcript& transcript) {
    if (!transcript.is_follow_up()) return transcript.text();
    return transcript.text() + " [follow-up replacing " + std::to_string(transcript.replaces_transcripts()) + "]";
}

using Shown = std::vector<std::string>;

class FollowUpTranscriptTest : public ::testing::Test {
protected:
    void SetUp() override {
        _dispatcher->addListener([this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (event.has_transcript() && event.transcript().is_final()) _transcripts.push_back(shown(event.transcript()));
            if (event.has_agent_state()) ++_states;
        });
        _assembler->setDownstream(_tap);
        _tap->initialize();
        _tap->start();
        _assembler->initialize();
        _assembler->start();
    }
    void TearDown() override {
        _assembler->stop();
        _tap->stop();
    }

    void send(std::shared_ptr<transport::Frame> frame) {
        _assembler->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    }
    void agent(Transition transition) {
        auto frame = std::make_shared<transport::AgentStateFrame>();
        frame->transition = transition;
        send(frame);
        ++_sentStates;
    }
    void stopped() { agent(Transition::USER_STOPPED_SPEAKING); }
    void transcript(const std::string& text, bool isFinal = true, bool isBackchannel = false) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = isFinal;
        frame->isBackchannel = isBackchannel;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 0.9f;
        send(frame);
    }
    void turnEnd() {
        send(std::make_shared<transport::TurnStateFrame>(transport::TurnStateFrame::TurnActor::USER,
                                                         transport::TurnStateFrame::TurnState::END,
                                                         transport::TurnStateFrame::TurnType::NORMAL));
    }

    // The final transcripts a UI got, once every frame sent has been through the tap.
    Shown finals(size_t expected) {
        EXPECT_TRUE(va::test::waitUntil([&] {
            std::lock_guard<std::mutex> lock(_mutex);
            return _transcripts.size() >= expected && _states >= _sentStates;
        })) << "the events reach the listener";
        std::lock_guard<std::mutex> lock(_mutex);
        return _transcripts;
    }

    std::mutex _mutex;
    Shown _transcripts;
    int _states = 0;
    int _sentStates = 0;
    std::shared_ptr<engine::EventDispatcher> _dispatcher = std::make_shared<engine::EventDispatcher>();
    std::shared_ptr<engine::EventTapProcessor> _tap = std::make_shared<engine::EventTapProcessor>(_dispatcher);
    std::shared_ptr<pipeline::TurnAssemblerProcessor> _assembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
};

TEST_F(FollowUpTranscriptTest, ResumedWhileThinkingReplacesBothParts) {
    stopped();
    transcript("Book a flight");                    // sent
    agent(Transition::USER_SPOKE_WHILE_THINKING);   // the user goes on
    stopped();
    transcript("to Paris");
    EXPECT_EQ(finals(3), (Shown{"Book a flight", "to Paris", "Book a flight to Paris [follow-up replacing 2]"}));
}

TEST_F(FollowUpTranscriptTest, WordsNeverSentAreReplacedToo) {
    transcript("Book a flight");                    // staged: the turn end never came
    agent(Transition::USER_STARTED_SPEAKING);       // the user goes on
    stopped();
    transcript("to Paris");
    EXPECT_EQ(finals(3), (Shown{"Book a flight", "to Paris", "Book a flight to Paris [follow-up replacing 2]"}));
}

TEST_F(FollowUpTranscriptTest, SecondResumeCountsTheFirstFollowUpAsOne) {
    stopped();
    transcript("Yeah.");
    agent(Transition::USER_SPOKE_WHILE_THINKING);
    stopped();
    transcript("Do you think");
    agent(Transition::USER_SPOKE_WHILE_THINKING);
    stopped();
    transcript("I have a contact named Heba?");
    EXPECT_EQ(finals(5), (Shown{"Yeah.", "Do you think", "Yeah. Do you think [follow-up replacing 2]",
                                "I have a contact named Heba?",
                                "Yeah. Do you think I have a contact named Heba? [follow-up replacing 2]"}))
        << "the second follow-up replaces the first one and its own part";
}

TEST_F(FollowUpTranscriptTest, LateFinalOfATurnSentOnItsPartialCountsForThatTurn) {
    transcript("Book a flight", false);
    agent(Transition::TRANSCRIPTION_READY);         // the eager policy sends the partial
    turnEnd();
    transcript("Book a flight.");                   // its late final: shown, not a new turn
    agent(Transition::USER_SPOKE_WHILE_THINKING);
    stopped();
    transcript("to Paris");
    EXPECT_EQ(finals(3), (Shown{"Book a flight.", "to Paris", "Book a flight to Paris [follow-up replacing 2]"}));
}

TEST_F(FollowUpTranscriptTest, BackChannelIsNotCounted) {
    stopped();
    transcript("Book a flight");
    transcript("mm-hmm", true, true);               // a back-channel: not one of the turn's words
    agent(Transition::USER_SPOKE_WHILE_THINKING);
    stopped();
    transcript("to Paris");
    EXPECT_EQ(finals(4), (Shown{"Book a flight", "mm-hmm", "to Paris", "Book a flight to Paris [follow-up replacing 2]"}));
}

TEST_F(FollowUpTranscriptTest, SeparateTurnsHaveNoFollowUp) {
    stopped();
    transcript("Book a flight");
    agent(Transition::SYNTHESIS_STARTED);
    agent(Transition::SYNTHESIS_COMPLETED);
    stopped();
    transcript("Thanks");
    EXPECT_EQ(finals(2), (Shown{"Book a flight", "Thanks"}));
}

} // namespace
