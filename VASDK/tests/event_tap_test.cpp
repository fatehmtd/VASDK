// EventTapProcessor's transcripts: a merged follow-up keeps its flag and how many transcripts it
// stands for, so a UI can show them as one message; an ordinary final has neither; a private
// answer's follow-up is masked like its parts. Its user_speech: a false start (the VAD falls quiet
// before it hears speech) is CANCELLED, so a "you're speaking" indicator goes away; speech that
// ends is ENDED once.

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

class EventTapTest : public ::testing::Test {
protected:
    // The transcripts the tap posts for each frame, in order.
    std::vector<vasdk::engine::Transcript> transcriptsFor(const std::vector<std::shared_ptr<transport::TranscriptionFrame>>& frames) {
        {
            engine::EventTapProcessor tap(_dispatcher);
            for (const auto& frame : frames) tap.processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
        }
        EXPECT_TRUE(va::test::waitUntil([&] {
            std::lock_guard<std::mutex> lock(_mutex);
            return _transcripts.size() >= frames.size();
        })) << "one transcript event per frame";
        std::lock_guard<std::mutex> lock(_mutex);
        return _transcripts;
    }

    // The user_speech states the tap posts for these VAD states, in order.
    std::vector<std::string> userSpeechFor(const std::vector<transport::VADState>& states, size_t expected) {
        {
            engine::EventTapProcessor tap(_dispatcher);
            for (const auto state : states) {
                auto frame = std::make_shared<transport::VADStateFrame>();
                frame->state = state;
                tap.processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
            }
        }
        EXPECT_TRUE(va::test::waitUntil([&] {
            std::lock_guard<std::mutex> lock(_mutex);
            return _userSpeech.size() >= expected;
        })) << "the user_speech events arrive";
        std::lock_guard<std::mutex> lock(_mutex);
        return _userSpeech;
    }

    static std::shared_ptr<transport::TranscriptionFrame> finalTranscript(const std::string& text) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = true;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 0.9f;
        return frame;
    }

    static std::shared_ptr<transport::TranscriptionFrame> followUp(const std::string& text, int replacesTranscripts) {
        auto frame = finalTranscript(text);
        frame->isFollowUp = true;
        frame->replacesTranscripts = replacesTranscripts;
        return frame;
    }

    std::mutex _mutex;
    std::vector<vasdk::engine::Transcript> _transcripts;
    std::vector<std::string> _userSpeech;
    std::shared_ptr<engine::EventDispatcher> _dispatcher = [this] {
        auto dispatcher = std::make_shared<engine::EventDispatcher>();
        dispatcher->addListener([this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (event.has_transcript()) _transcripts.push_back(event.transcript());
            if (event.has_user_speech()) _userSpeech.push_back(vasdk::engine::UserSpeech::State_Name(event.user_speech().state()));
        });
        return dispatcher;
    }();
};

TEST_F(EventTapTest, FollowUpCarriesTheTranscriptsItReplaces) {
    const auto transcripts = transcriptsFor({finalTranscript("to Paris"), followUp("Book a flight to Paris", 2)});
    ASSERT_EQ(transcripts.size(), 2u);
    EXPECT_EQ(transcripts[1].text(), "Book a flight to Paris");
    EXPECT_TRUE(transcripts[1].is_final());
    EXPECT_TRUE(transcripts[1].is_follow_up()) << "the merged text is marked as a follow-up";
    EXPECT_EQ(transcripts[1].replaces_transcripts(), 2) << "it stands for \"Book a flight\" and \"to Paris\"";
}

TEST_F(EventTapTest, OrdinaryFinalIsNoFollowUp) {
    const auto transcripts = transcriptsFor({finalTranscript("Book a flight")});
    ASSERT_EQ(transcripts.size(), 1u);
    EXPECT_FALSE(transcripts[0].is_follow_up());
    EXPECT_EQ(transcripts[0].replaces_transcripts(), 0);
}

TEST_F(EventTapTest, PrivateFollowUpIsMasked) {
    auto merged = followUp("5551234567", 2);
    merged->sensitive = true;
    const auto transcripts = transcriptsFor({merged});
    ASSERT_EQ(transcripts.size(), 1u);
    EXPECT_EQ(transcripts[0].text(), "******4567") << "a private answer stays masked when merged";
    EXPECT_TRUE(transcripts[0].is_follow_up());
}

using States = std::vector<std::string>;
using Vad = transport::VADState;

TEST_F(EventTapTest, FalseStartIsCancelled) {
    EXPECT_EQ(userSpeechFor({Vad::STARTING, Vad::QUIET}, 2), (States{"STARTED", "CANCELLED"}))
        << "the VAD fell quiet before it heard speech";
}

TEST_F(EventTapTest, SpeechEndsOnce) {
    EXPECT_EQ(userSpeechFor({Vad::STARTING, Vad::ONGOING, Vad::PAUSED, Vad::RESUMING, Vad::ENDED, Vad::QUIET}, 4),
              (States{"STARTED", "ONGOING", "ONGOING", "ENDED"}))
        << "QUIET after ENDED adds nothing";
}

TEST_F(EventTapTest, ConfirmedSpeechFallingQuietEnds) {
    EXPECT_EQ(userSpeechFor({Vad::STARTING, Vad::ONGOING, Vad::QUIET}, 3), (States{"STARTED", "ONGOING", "ENDED"}))
        << "speech that ends without an ENDED still ends, it isn't cancelled";
}

TEST_F(EventTapTest, QuietWithoutSpeechPostsNothing) {
    EXPECT_EQ(userSpeechFor({Vad::QUIET, Vad::STARTING, Vad::QUIET, Vad::QUIET}, 2), (States{"STARTED", "CANCELLED"}));
}

} // namespace
