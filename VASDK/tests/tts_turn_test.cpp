// TTSFrameProcessor turn model: a reply streamed as several TextFrames yields exactly one
// SynthesisState STARTED and one COMPLETED (after its audio played), for a provider that queues
// calls and for one that must get them one at a time; a barge-in drops the stale reply, even
// when the provider confirms the cancel synchronously. A Pause hold during a pending barge-in
// delays COMPLETED by the paused time, and every hold is released. A part that can't be
// interrupted is synthesized on its own and reported only while its audio plays.

#include <VASDK/TTSFrameProcessor.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace va::transport {
// The states by name in a failure's report
void PrintTo(SynthesisState state, std::ostream* out) { *out << toString(state); }
} // namespace va::transport

using namespace va;
using namespace std::chrono_literals;
using transport::SynthesisState;
using va::test::waitUntil;

namespace {

using States = std::vector<SynthesisState>;

// Milliseconds from `from` to `to`, a number a failed check prints
long long millisecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

// Behaves like the local providers: a worker drains a FIFO, fires Started when it goes busy and
// Completed only when the queue runs dry. Each sentence yields 100 ms of 16 kHz audio.
class FakeProvider : public audio::GenericVoiceSynthesisProvider {
public:
    FakeProvider(bool queued, bool syncCompletedOnCancel) : _queued(queued), _syncCancel(syncCompletedOnCancel) {}
    ~FakeProvider() override { stopVoiceSynthesisProvider(); }

    bool synthesizeSpeech(const std::string& text) override {
        std::lock_guard<std::mutex> l(_m);
        if (!_queued && (_busy || !_queue.empty())) violations++;
        received.push_back(text);
        _queue.push_back(text);
        _cv.notify_all();
        return true;
    }
    void cancelSynthesis() override {
        {
            std::lock_guard<std::mutex> l(_m);
            _queue.clear();
            _cancelled = true;
        }
        if (_syncCancel && _listener) _listener->onSynthesisCompleted();  // like the cloud providers
    }
    bool acceptsQueuedSynthesis() const override { return _queued; }
    bool startVoiceSynthesisProvider() override {
        _running = true;
        _worker = std::thread([this] { loop(); });
        return true;
    }
    void stopVoiceSynthesisProvider() override {
        {
            std::lock_guard<std::mutex> l(_m);
            if (!_running) return;
            _running = false;
        }
        _cv.notify_all();
        if (_worker.joinable()) _worker.join();
    }
    bool isVoiceSynthesisProviderRunning() const override { return _running; }
    void setAudioConfiguration(const audio::AudioConfig& c) override { _cfg = c; }
    audio::AudioConfig getAudioConfiguration() const override { return _cfg; }
    void setVoiceSynthesisResultListener(audio::VoiceSynthesisResultListener* l) override { _listener = l; }
    audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override { return _listener; }

    std::vector<std::string> received;
    int violations = 0;

private:
    void loop() {
        std::unique_lock<std::mutex> l(_m);
        while (true) {
            _cv.wait(l, [this] { return !_running || !_queue.empty(); });
            if (!_running) return;
            _busy = true;
            _cancelled = false;
            l.unlock();
            _listener->onSynthesisStarted();
            l.lock();
            while (!_queue.empty() && !_cancelled) {
                _queue.pop_front();
                l.unlock();
                std::vector<char> pcm(3200, 1);  // 100 ms of 16 kHz mono int16
                _listener->onAudioDataReceived(pcm.data(), pcm.size());
                std::this_thread::sleep_for(20ms);  // fixed wait: the fake's synthesis time
                l.lock();
            }
            _busy = false;
            const bool cancelled = _cancelled;
            l.unlock();
            if (!cancelled) _listener->onSynthesisCompleted();
            l.lock();
        }
    }

    bool _queued, _syncCancel;
    audio::AudioConfig _cfg{};
    audio::VoiceSynthesisResultListener* _listener = nullptr;
    std::mutex _m;
    std::condition_variable _cv;
    std::deque<std::string> _queue;
    bool _busy = false, _cancelled = false;
    std::atomic<bool> _running{false};
    std::thread _worker;
};

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection) override {
        if (f->getType() != transport::FrameType::SYNTHESIS_STATE) return;
        std::lock_guard<std::mutex> l(m);
        states.push_back(std::static_pointer_cast<transport::SynthesisStateFrame>(f)->state);
        times.push_back(std::chrono::steady_clock::now());
    }
    States snapshot() {
        std::lock_guard<std::mutex> l(m);
        return states;
    }
    std::mutex m;
    States states;
    std::vector<std::chrono::steady_clock::time_point> times;
};

std::shared_ptr<transport::TextFrame> text(const std::string& t, bool isFinal) {
    auto f = std::make_shared<transport::TextFrame>();
    f->text = t;
    f->isFinal = isFinal;
    return f;
}

std::shared_ptr<transport::TextFrame> reply(const std::string& t, bool isFinal, uint64_t turnId) {
    auto f = text(t, isFinal);
    f->turnId = turnId;
    return f;
}

std::shared_ptr<transport::TurnReplacedFrame> replaced(uint64_t turnId) {
    auto f = std::make_shared<transport::TurnReplacedFrame>();
    f->turnId = turnId;
    return f;
}

std::shared_ptr<transport::AgentStateFrame> agent(transport::AgentStateTransition t) {
    auto f = std::make_shared<transport::AgentStateFrame>();
    f->transition = t;
    return f;
}

// The processor under test, its provider and what it reported, started; stopped when it goes.
struct Rig {
    std::shared_ptr<FakeProvider> provider;
    std::shared_ptr<pipeline::TTSFrameProcessor> tts;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();

    Rig(bool queued, bool syncCancel) : provider(std::make_shared<FakeProvider>(queued, syncCancel)) {
        audio::AudioConfig cfg{};
        cfg.sampleRate = 16000;
        cfg.format     = audio::AudioFormat::SHORT;
        cfg.channels   = 1;
        provider->setAudioConfiguration(cfg);
        tts = std::make_shared<pipeline::TTSFrameProcessor>(provider);
        tts->setHardwareBufferMarginMs(20);
        tts->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        tts->initialize();
        tts->start();
    }
    ~Rig() { tts->stop(); recorder->stop(); }
    void send(std::shared_ptr<transport::Frame> f) { tts->processFrame(f, pipeline::FlowDirection::DOWNSTREAM); }
};

// A provider that queues calls, and one that must get them one at a time.
class TtsTurnProviderTest : public testing::TestWithParam<bool> {
protected:
    bool queued() const { return GetParam(); }
};

std::string providerName(const testing::TestParamInfo<bool>& info) { return info.param ? "Queued" : "OneAtATime"; }

TEST_P(TtsTurnProviderTest, StreamedReplyIsOneTurn) {
    Rig rig(queued(), false);
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("One. ", false));
    rig.send(text("Two. ", false));
    std::this_thread::sleep_for(300ms);  // fixed wait: the provider goes idle mid-reply (LLM slower than TTS)
    rig.send(text("Three.", true));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; }, 5s);

    const auto states = rig.recorder->snapshot();
    EXPECT_EQ(states, (States{SynthesisState::STARTED, SynthesisState::COMPLETED})) << "exactly one STARTED then one COMPLETED";
    EXPECT_EQ(rig.provider->received, (std::vector<std::string>{"One. ", "Two. ", "Three."})) << "all sentences in order";
    EXPECT_EQ(rig.provider->violations, 0) << "no call while the provider was busy";
    if (states.size() == 2) {
        // 300 ms of audio, the last 100 ms starting after the 300 ms gap.
        EXPECT_GE(millisecondsBetween(t0, rig.recorder->times[1]), 400) << "COMPLETED only after the last sentence played";
    }
}

TEST_P(TtsTurnProviderTest, UninterruptiblePartIsReportedOnlyWhilePlaying) {
    Rig rig(queued(), false);
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("Intro.", false));
    auto disclaimer = text("Disclaimer.", false);
    disclaimer->interruptible = false;
    rig.send(disclaimer);
    rig.send(text("Question?", true));
    // Each sentence is 100 ms of audio, played back to back: the disclaimer at 100-200 ms, heard 20 ms later.
    // Sampled rather than read at fixed times: a loaded machine runs late, never early.
    std::chrono::steady_clock::time_point on{}, off{};
    while (std::chrono::steady_clock::now() < t0 + 5000ms && off == std::chrono::steady_clock::time_point{}) {
        const bool protectedNow = rig.tts->getProtectedPlaybackMs() > 0;
        const auto now = std::chrono::steady_clock::now();
        if (protectedNow && on == std::chrono::steady_clock::time_point{}) on = now;
        if (!protectedNow && on != std::chrono::steady_clock::time_point{}) off = now;
        std::this_thread::sleep_for(2ms);  // fixed wait: the sampling period
    }
    ASSERT_TRUE(on != std::chrono::steady_clock::time_point{}) << "reported while it plays";
    EXPECT_GE(millisecondsBetween(t0, on), 80) << "not during the part before";
    EXPECT_TRUE(off != std::chrono::steady_clock::time_point{}) << "not during the part after";
    waitUntil([&] { return rig.provider->received.size() >= 3 && rig.recorder->snapshot().size() >= 2; }, 5s);
    EXPECT_EQ(rig.provider->received, (std::vector<std::string>{"Intro.", "Disclaimer.", "Question?"})) << "all parts in order";
    EXPECT_EQ(rig.recorder->snapshot(), (States{SynthesisState::STARTED, SynthesisState::COMPLETED})) << "still one turn";
}

INSTANTIATE_TEST_SUITE_P(Providers, TtsTurnProviderTest, testing::Bool(), providerName);

// A provider that confirms a cancel later, and one that confirms it synchronously (the cloud ones).
class TtsTurnBargeInTest : public testing::TestWithParam<bool> {
protected:
    bool syncCancel() const { return GetParam(); }
};

std::string cancelName(const testing::TestParamInfo<bool>& info) { return info.param ? "SyncCancel" : "LaterCancel"; }

TEST_P(TtsTurnBargeInTest, DropsTheStaleReply) {
    Rig rig(true, syncCancel());
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("Alpha. ", false));
    std::this_thread::sleep_for(50ms);  // fixed wait: the user cuts in 50 ms into Alpha
    if (std::chrono::steady_clock::now() - t0 > 90ms)   // Alpha's 100 ms may have played out: no barge-in to test
        GTEST_SKIP() << "the machine overslept";
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT));
    rig.send(text("Beta.", true));  // stale sentence of the interrupted reply
    std::this_thread::sleep_for(100ms);  // fixed wait: the interruption lasts 100 ms
    rig.send(agent(transport::AgentStateTransition::INTERRUPT_RESOLVED));
    rig.send(text("Gamma.", true));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 4; }, 5s);

    EXPECT_EQ(rig.recorder->snapshot(), (States{SynthesisState::STARTED, SynthesisState::INTERRUPTED,
                                                SynthesisState::STARTED, SynthesisState::COMPLETED}))
        << "STARTED, INTERRUPTED, then a clean new turn";
    EXPECT_EQ(rig.provider->received, (std::vector<std::string>{"Alpha. ", "Gamma."})) << "stale sentence dropped";
}

INSTANTIATE_TEST_SUITE_P(Cancels, TtsTurnBargeInTest, testing::Bool(), cancelName);

TEST(TtsTurn, PauseHoldDelaysCompleted) {
    std::mutex holdMutex;   // before the rig: stopping it may still report a hold
    std::vector<bool> holds;
    Rig rig(true, false);
    rig.tts->setPendingPlayback(transport::PendingPlaybackMode::Pause, -12.0f);
    rig.tts->setPlaybackHold([&](transport::PendingPlaybackMode playbackMode, bool held, float) {
        std::lock_guard<std::mutex> lock(holdMutex);
        if (playbackMode == transport::PendingPlaybackMode::Pause) holds.push_back(held);
    });
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("One. ", false));
    rig.send(text("Two.", true));
    // Text complete but still synthesizing: unknown, not "0 ms left" (that read as the tail and blocked barge-in)
    EXPECT_EQ(rig.tts->getRemainingReplyPlaybackMs(), -1) << "still synthesizing: remaining playback unknown";
    // Synthesis takes about 40 ms here; wait for it rather than for a fixed time (Windows timers tick every 15.6 ms).
    // The remaining playback is known only until the 200 ms reply has played: a thread that oversleeps
    // past it finds the turn over (COMPLETED), with no reply left to pause.
    waitUntil([&] { return rig.tts->getRemainingReplyPlaybackMs() >= 0 || rig.recorder->snapshot().size() >= 2; }, 5s);
    if (rig.recorder->snapshot().size() >= 2) GTEST_SKIP() << "the machine overslept";
    EXPECT_GE(rig.tts->getRemainingReplyPlaybackMs(), 0) << "reply text complete: remaining playback known";
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    if (rig.recorder->snapshot().size() >= 2)   // the reply ended between the look and the barge-in
        GTEST_SKIP() << "the machine overslept";
    std::this_thread::sleep_for(400ms);  // fixed wait: longer than the 200 ms reply, only the pause keeps it going
    EXPECT_EQ(rig.recorder->snapshot(), (States{SynthesisState::STARTED})) << "no COMPLETED while paused";
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; }, 5s);

    const auto states = rig.recorder->snapshot();
    EXPECT_EQ(states, (States{SynthesisState::STARTED, SynthesisState::COMPLETED})) << "one STARTED, one COMPLETED";
    if (states.size() == 2) EXPECT_GE(millisecondsBetween(t0, rig.recorder->times[1]), 560) << "COMPLETED is pushed back by the paused time";
    std::lock_guard<std::mutex> lock(holdMutex);
    EXPECT_EQ(holds, (std::vector<bool>{true, false})) << "held once, released once";
}

// The duck holds, and when each was released: a duck lasts at least its minimum duration (300 ms here).
class TtsTurnDuckTest : public testing::Test {
protected:
    TtsTurnDuckTest() {
        rig.tts->setPendingPlayback(transport::PendingPlaybackMode::Duck, -9.0f, 300);
        rig.tts->setPlaybackHold([this](transport::PendingPlaybackMode playbackMode, bool held, float duckGainDecibels) {
            std::lock_guard<std::mutex> lock(_holdMutex);
            if (playbackMode == transport::PendingPlaybackMode::Duck && duckGainDecibels == -9.0f)
                _holds.emplace_back(held, std::chrono::steady_clock::now());
        });
    }
    std::vector<std::pair<bool, std::chrono::steady_clock::time_point>> holds() {
        std::lock_guard<std::mutex> lock(_holdMutex);
        return _holds;
    }
    void send(transport::AgentStateTransition transition) { rig.send(agent(transition)); }

private:
    // Before the rig: stopping it may still report a hold
    std::mutex _holdMutex;
    std::vector<std::pair<bool, std::chrono::steady_clock::time_point>> _holds;

protected:
    Rig rig{true, false};
};

TEST_F(TtsTurnDuckTest, LastsTheMinimumDuration) {
    // Back-channel dismissed after 50 ms: the duck still lasts 300 ms from its start.
    const auto duckStart = std::chrono::steady_clock::now();
    send(transport::AgentStateTransition::BARGE_IN_PENDING);
    std::this_thread::sleep_for(50ms);  // fixed wait: the back-channel lasts 50 ms
    send(transport::AgentStateTransition::BACKCHANNEL);
    std::this_thread::sleep_for(150ms);  // fixed wait: checking no release comes before the minimum duration
    const bool stillShort = std::chrono::steady_clock::now() - duckStart < 280ms;   // a loaded machine may oversleep
    const auto early = holds();
    EXPECT_TRUE(early.size() == 1 || (!stillShort && early.size() == 2)) << "still ducked before the minimum duration";
    waitUntil([&] { return holds().size() >= 2; }, 5s);
    const auto events = holds();
    ASSERT_EQ(events.size(), 2u) << "released once the minimum duration passed";
    EXPECT_FALSE(events[1].first) << "the second hold is the release";
    // Not much longer than 300 ms: a release that waited for anything else would be far later. A
    // loaded machine wakes the release late, so the bound isn't tight.
    const auto heldFor = millisecondsBetween(events[0].second, events[1].second);
    EXPECT_GE(heldFor, 290) << "held for about the minimum duration";
    EXPECT_LT(heldFor, 600) << "held for about the minimum duration";
}

TEST_F(TtsTurnDuckTest, NewOverlapCancelsTheScheduledRelease) {
    // Speech over the agent again while the release is pending: stays ducked from the new start.
    send(transport::AgentStateTransition::BARGE_IN_PENDING);
    const auto start = std::chrono::steady_clock::now();
    send(transport::AgentStateTransition::BACKCHANNEL);
    std::this_thread::sleep_for(200ms);  // fixed wait: the release is still 100 ms away
    if (std::chrono::steady_clock::now() - start >= 280ms) GTEST_SKIP() << "the machine overslept";
    send(transport::AgentStateTransition::BARGE_IN_PENDING);
    std::this_thread::sleep_for(200ms);  // fixed wait: checking the cancelled release doesn't come
    EXPECT_EQ(holds().size(), 2u) << "a new overlap cancels the scheduled release";
}

TEST_F(TtsTurnDuckTest, InterruptResolutionReleasesAtOnce) {
    // A committed barge-in releases as soon as the interrupt resolves, without waiting.
    send(transport::AgentStateTransition::BARGE_IN_PENDING);
    send(transport::AgentStateTransition::BARGE_IN_INTERRUPT);
    send(transport::AgentStateTransition::INTERRUPT_RESOLVED);
    waitUntil([&] { return holds().size() >= 2; }, 1000ms);
    const auto events = holds();
    ASSERT_EQ(events.size(), 2u) << "interrupt resolution releases at once";
    EXPECT_FALSE(events.back().first) << "the second hold is the release";
    std::this_thread::sleep_for(400ms);  // fixed wait: checking no stray release comes later
    EXPECT_EQ(holds().size(), 2u) << "no stray release later";
}

// The user resumed turn 3: what's queued of its reply goes, and other speech stays.
TEST(TtsTurn, ReplacedTurnDropsItsQueuedSentences) {
    Rig rig(false, false);   // one sentence at a time: the reply's waits in the queue
    rig.send(text("Intro. ", false));          // not a reply: never dropped
    rig.send(reply("Got it. ", false, 3));
    rig.send(reply("Anything else?", false, 3));
    rig.send(replaced(3));                     // its final never comes
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; }, 5s);
    EXPECT_EQ(rig.provider->received, (std::vector<std::string>{"Intro. "})) << "the replaced turn's sentences dropped";
    EXPECT_EQ(rig.recorder->snapshot(), (States{SynthesisState::STARTED, SynthesisState::COMPLETED})) << "the turn still completes";
}

// Turn 3's reply is playing: it stops, the turn completes (no INTERRUPTED: the user didn't cut in),
// its late text is dropped, and turn 4's reply is a clean new turn.
TEST(TtsTurn, ReplacedTurnStopsThePlayingReply) {
    Rig rig(true, false);
    rig.send(reply("Alpha. ", false, 3));
    std::this_thread::sleep_for(30ms);  // fixed wait: the user resumes 30 ms into Alpha
    rig.send(replaced(3));
    rig.send(reply("Beta.", true, 3));          // the stale reply's rest, arriving after
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; }, 5s);
    rig.send(reply("Gamma.", true, 4));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 4; }, 5s);
    EXPECT_EQ(rig.provider->received, (std::vector<std::string>{"Alpha. ", "Gamma."})) << "late text of the replaced turn dropped";
    EXPECT_EQ(rig.recorder->snapshot(), (States{SynthesisState::STARTED, SynthesisState::COMPLETED,
                                                SynthesisState::STARTED, SynthesisState::COMPLETED}))
        << "stopped turn completes, then a clean new one";
}

} // namespace
