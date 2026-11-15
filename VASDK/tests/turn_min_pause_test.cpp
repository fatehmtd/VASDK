// TurnDetectionProcessor's minimum pause (turn_taking.min_pause_ms), for any turn model: a turn end
// the model calls a moment into a pause is held until the pause has lasted, then sent (and the VAD
// asked to close the segment); dropped when the user speaks again, the VAD's end then ending the
// turn once; sent when the VAD ends first. Without one, the model's call ends the turn at once.

#include <VASDK/TurnDetectionProcessor.h>

#include "support/TestSupport.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace va;
using Vad = transport::VADState;

namespace {

// Records the user's turn ends, and every frame it gets.
class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame);
        if (auto turn = std::dynamic_pointer_cast<transport::TurnStateFrame>(frame);
            turn && turn->actor == transport::TurnStateFrame::TurnActor::USER && turn->state == transport::TurnStateFrame::TurnState::END)
            ++userTurnEnds;
    }
    bool saw(const std::shared_ptr<transport::Frame>& frame) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& seen : frames) if (seen == frame) return true;
        return false;
    }
    int ends() {
        std::lock_guard<std::mutex> lock(mutex);
        return userTurnEnds;
    }
    std::mutex mutex;
    std::vector<std::shared_ptr<transport::Frame>> frames;
    int userTurnEnds = 0;
};

// Calls the user's turn over at the first pause of each segment, as turn-1-mini does a moment into it.
class OncePerSegmentModel : public audio::TurnDetectionProvider {
public:
    TurnEvaluation evaluate(const std::shared_ptr<transport::Frame> frame, bool) override {
        TurnEvaluation evaluation;
        if (frame->getType() != transport::FrameType::VAD_STATE) return evaluation;
        const auto state = std::static_pointer_cast<transport::VADStateFrame>(frame)->state;
        if (state == Vad::STARTING) _called = false;
        if (state == Vad::PAUSED && !_called) {
            _called = true;
            evaluation.decision = Decision::TURN_END;
            evaluation.confidence = 0.8f;
        }
        return evaluation;
    }

private:
    bool _called = false;
};

class TurnMinPauseTest : public ::testing::Test {
protected:
    void SetUp() override {
        _turn->setDownstream(_downstream);
        _turn->setUpstream(_upstream);
        for (auto& processor : {_downstream, _upstream}) {
            processor->initialize();
            processor->start();
        }
        _turn->initialize();
        _turn->start();
    }
    void TearDown() override {
        _turn->stop();
        _downstream->stop();
        _upstream->stop();
    }

    void vad(Vad state) {
        auto frame = std::make_shared<transport::VADStateFrame>();
        frame->state = state;
        send(frame);
    }
    void audio(int ms) {
        auto frame = std::make_shared<transport::AudioFrame>();
        frame->sampleRate = 16000;
        frame->channels = 1;
        frame->numSamples = static_cast<uint32_t>(16 * ms);
        send(frame);
    }
    void send(std::shared_ptr<transport::Frame> frame) {
        _last = frame;
        _turn->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    }
    // The user's turn ends sent downstream, once every frame sent has gone through.
    int userTurnEnds() {
        EXPECT_TRUE(va::test::waitUntil([&] { return _downstream->saw(_last); })) << "every frame passes through";
        return _downstream->ends();
    }

    std::shared_ptr<pipeline::TurnDetectionProcessor> _turn =
        std::make_shared<pipeline::TurnDetectionProcessor>(std::make_shared<OncePerSegmentModel>());
    std::shared_ptr<Recorder> _downstream = std::make_shared<Recorder>();
    std::shared_ptr<Recorder> _upstream = std::make_shared<Recorder>();
    std::shared_ptr<transport::Frame> _last;
};

TEST_F(TurnMinPauseTest, WithoutOneTheModelEndsTheTurnAtOnce) {
    vad(Vad::STARTING);
    vad(Vad::ONGOING);
    vad(Vad::PAUSED);
    EXPECT_EQ(userTurnEnds(), 1) << "ended at the pause's start";
    EXPECT_TRUE(va::test::waitUntil([&] { return _upstream->ends() == 1; })) << "the VAD is asked to close the segment";
}

TEST_F(TurnMinPauseTest, HeldUntilThePauseLasts) {
    _turn->setMinPauseMs(300);
    vad(Vad::STARTING);
    vad(Vad::ONGOING);
    vad(Vad::PAUSED);
    audio(100);
    audio(100);
    EXPECT_EQ(userTurnEnds(), 0) << "200 ms is a breath";
    audio(100);
    EXPECT_EQ(userTurnEnds(), 1) << "sent once the pause has lasted 300 ms";
    EXPECT_TRUE(va::test::waitUntil([&] { return _upstream->ends() == 1; })) << "the VAD is asked to close the segment";
}

TEST_F(TurnMinPauseTest, DroppedWhenTheUserGoesOn) {
    _turn->setMinPauseMs(300);
    vad(Vad::STARTING);
    vad(Vad::ONGOING);
    vad(Vad::PAUSED);
    audio(100);
    vad(Vad::RESUMING);   // "Book a flight ... to Paris"
    vad(Vad::ONGOING);
    audio(400);
    EXPECT_EQ(userTurnEnds(), 0) << "the user went on: not the end";
    vad(Vad::PAUSED);     // the model, having called it, stays quiet
    audio(400);
    vad(Vad::ENDING);
    vad(Vad::ENDED);
    EXPECT_EQ(userTurnEnds(), 1) << "the VAD's end ends the turn, once";
}

TEST_F(TurnMinPauseTest, VadEndFirstEndsTheHeldTurn) {
    _turn->setMinPauseMs(1000);
    vad(Vad::STARTING);
    vad(Vad::ONGOING);
    vad(Vad::PAUSED);
    audio(100);
    vad(Vad::ENDING);
    vad(Vad::ENDED);
    EXPECT_EQ(userTurnEnds(), 1) << "the silence ended it before the minimum pause";
}

TEST_F(TurnMinPauseTest, PausedThenEndingIsOnePause) {
    _turn->setMinPauseMs(300);
    vad(Vad::STARTING);
    vad(Vad::ONGOING);
    vad(Vad::PAUSED);
    audio(200);
    vad(Vad::ENDING);     // the same silence, longer
    audio(100);
    EXPECT_EQ(userTurnEnds(), 1) << "the pause counts from PAUSED, not from ENDING";
}

} // namespace
