// TTSFrameProcessor turn model: a reply streamed as several TextFrames yields exactly one
// SynthesisState STARTED and one COMPLETED (after its audio played), for a provider that queues
// calls and for one that must get them one at a time; a barge-in drops the stale reply, even
// when the provider confirms the cancel synchronously. A Pause hold during a pending barge-in
// delays COMPLETED by the paused time, and every hold is released. A part that can't be
// interrupted is synthesized on its own and reported only while its audio plays.

#include <VASDK/TTSFrameProcessor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using transport::SynthesisState;

namespace {

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
                std::this_thread::sleep_for(20ms);
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
    std::vector<SynthesisState> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return states;
    }
    std::mutex m;
    std::vector<SynthesisState> states;
    std::vector<std::chrono::steady_clock::time_point> times;
};

std::shared_ptr<transport::TextFrame> text(const std::string& t, bool isFinal) {
    auto f = std::make_shared<transport::TextFrame>();
    f->text = t;
    f->isFinal = isFinal;
    return f;
}

std::shared_ptr<transport::AgentStateFrame> agent(transport::AgentStateTransition t) {
    auto f = std::make_shared<transport::AgentStateFrame>();
    f->transition = t;
    return f;
}

// Until `condition` holds, at most `timeout`. A check that something happens waits for it, not a
// fixed time: a loaded machine (a shared CI runner) runs late. Lower bounds on timings stay as they
// are (late never makes them fail); a check that something doesn't happen still waits a fixed time.
template <class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

int failures = 0;
void check(bool ok, const char* what, const char* mode) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL [%s] %s\n", mode, what); }
}

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

void streamedReply(bool queued) {
    const char* mode = queued ? "queued" : "sequential";
    Rig rig(queued, false);
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("One. ", false));
    rig.send(text("Two. ", false));
    std::this_thread::sleep_for(300ms);  // provider goes idle mid-reply (LLM slower than TTS)
    rig.send(text("Three.", true));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; });

    const auto states = rig.recorder->snapshot();
    check(states == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::COMPLETED},
          "exactly one STARTED then one COMPLETED", mode);
    check(rig.provider->received == std::vector<std::string>{"One. ", "Two. ", "Three."}, "all sentences in order", mode);
    check(rig.provider->violations == 0, "no call while the provider was busy", mode);
    if (states.size() == 2) {
        // 300 ms of audio, the last 100 ms starting after the 300 ms gap.
        const auto completedAfter = std::chrono::duration_cast<std::chrono::milliseconds>(rig.recorder->times[1] - t0);
        check(completedAfter >= 400ms, "COMPLETED only after the last sentence played", mode);
    }
}

void bargeIn(bool syncCancel) {
    const char* mode = syncCancel ? "barge-in, sync cancel" : "barge-in";
    Rig rig(true, syncCancel);
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("Alpha. ", false));
    std::this_thread::sleep_for(50ms);
    if (std::chrono::steady_clock::now() - t0 > 90ms) {   // Alpha's 100 ms may have played out: no barge-in to test
        std::printf("[%s] skipped: the machine overslept\n", mode);
        return;
    }
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT));
    rig.send(text("Beta.", true));  // stale sentence of the interrupted reply
    std::this_thread::sleep_for(100ms);
    rig.send(agent(transport::AgentStateTransition::INTERRUPT_RESOLVED));
    rig.send(text("Gamma.", true));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 4; });

    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::INTERRUPTED,
                                                                 SynthesisState::STARTED, SynthesisState::COMPLETED},
          "STARTED, INTERRUPTED, then a clean new turn", mode);
    check(rig.provider->received == std::vector<std::string>{"Alpha. ", "Gamma."}, "stale sentence dropped", mode);
}

void protectedPart(bool queued) {
    const char* mode = queued ? "can't be interrupted, queued" : "can't be interrupted, sequential";
    Rig rig(queued, false);
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
        std::this_thread::sleep_for(2ms);
    }
    check(on != std::chrono::steady_clock::time_point{}, "while it plays", mode);
    check(on == std::chrono::steady_clock::time_point{} || on - t0 >= 80ms, "not during the part before", mode);
    check(off != std::chrono::steady_clock::time_point{}, "not during the part after", mode);
    waitUntil([&] { return rig.provider->received.size() >= 3 && rig.recorder->snapshot().size() >= 2; });
    check(rig.provider->received == std::vector<std::string>{"Intro.", "Disclaimer.", "Question?"}, "all parts in order", mode);
    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::COMPLETED},
          "still one turn", mode);
}

void pausedReply() {
    const char* mode = "pause hold";
    Rig rig(true, false);
    std::mutex holdMutex;
    std::vector<bool> holds;
    rig.tts->setPendingPlayback(transport::PendingPlaybackMode::Pause, -12.0f);
    rig.tts->setPlaybackHold([&](transport::PendingPlaybackMode playbackMode, bool held, float) {
        std::lock_guard<std::mutex> lock(holdMutex);
        if (playbackMode == transport::PendingPlaybackMode::Pause) holds.push_back(held);
    });
    const auto t0 = std::chrono::steady_clock::now();
    rig.send(text("One. ", false));
    rig.send(text("Two.", true));
    // Text complete but still synthesizing: unknown, not "0 ms left" (that read as the tail and blocked barge-in)
    check(rig.tts->getRemainingReplyPlaybackMs() == -1, "still synthesizing: remaining playback unknown", mode);
    // Synthesis takes about 40 ms here; wait for it rather than for a fixed time (Windows timers tick every 15.6 ms).
    // The remaining playback is known only until the 200 ms reply has played: a thread that oversleeps
    // past it finds the turn over (COMPLETED), with no reply left to pause.
    waitUntil([&] { return rig.tts->getRemainingReplyPlaybackMs() >= 0 || rig.recorder->snapshot().size() >= 2; });
    const auto overslept = [&] {
        if (rig.recorder->snapshot().size() < 2) return false;
        std::printf("[%s] skipped: the machine overslept\n", mode);
        return true;
    };
    if (overslept()) return;
    check(rig.tts->getRemainingReplyPlaybackMs() >= 0, "reply text complete: remaining playback known", mode);
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    if (overslept()) return;  // the reply ended between the look and the barge-in
    std::this_thread::sleep_for(400ms);  // longer than the 200 ms reply: only the pause keeps it going
    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED}, "no COMPLETED while paused", mode);
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; });

    const auto states = rig.recorder->snapshot();
    check(states == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::COMPLETED},
          "one STARTED, one COMPLETED", mode);
    if (states.size() == 2) {
        const auto completedAfter = std::chrono::duration_cast<std::chrono::milliseconds>(rig.recorder->times[1] - t0);
        check(completedAfter >= 560ms, "COMPLETED is pushed back by the paused time", mode);
    }
    std::lock_guard<std::mutex> lock(holdMutex);
    check(holds == std::vector<bool>{true, false}, "held once, released once", mode);
}

void duckLastsMinimumDuration() {
    const char* mode = "duck duration";
    Rig rig(true, false);
    std::mutex holdMutex;
    std::vector<std::pair<bool, std::chrono::steady_clock::time_point>> holds;
    rig.tts->setPendingPlayback(transport::PendingPlaybackMode::Duck, -9.0f, 300);
    rig.tts->setPlaybackHold([&](transport::PendingPlaybackMode playbackMode, bool held, float duckGainDecibels) {
        std::lock_guard<std::mutex> lock(holdMutex);
        if (playbackMode == transport::PendingPlaybackMode::Duck && duckGainDecibels == -9.0f)
            holds.emplace_back(held, std::chrono::steady_clock::now());
    });
    auto snapshot = [&] { std::lock_guard<std::mutex> lock(holdMutex); return holds; };

    // Back-channel dismissed after 50 ms: the duck still lasts 300 ms from its start.
    const auto duckStart = std::chrono::steady_clock::now();
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    std::this_thread::sleep_for(50ms);
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    std::this_thread::sleep_for(150ms);
    const bool stillShort = std::chrono::steady_clock::now() - duckStart < 280ms;   // a loaded machine may oversleep
    const auto early = snapshot();
    check(early.size() == 1 || (!stillShort && early.size() == 2), "still ducked before the minimum duration", mode);
    waitUntil([&] { return snapshot().size() >= 2; });
    auto events = snapshot();
    check(events.size() == 2 && !events[1].first, "released once the minimum duration passed", mode);
    if (events.size() == 2) {
        // Not much longer than 300 ms: a release that waited for anything else would be far later. A
        // loaded machine wakes the release late, so the bound isn't tight.
        const auto heldFor = std::chrono::duration_cast<std::chrono::milliseconds>(events[1].second - events[0].second);
        check(heldFor >= 290ms && heldFor < 600ms, "held for about the minimum duration", mode);
    }

    // Speech over the agent again while the release is pending: stays ducked from the new start.
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    const auto secondStart = std::chrono::steady_clock::now();
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    std::this_thread::sleep_for(200ms);
    const bool beforeRelease = std::chrono::steady_clock::now() - secondStart < 280ms;   // a loaded machine may oversleep
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    std::this_thread::sleep_for(200ms);
    if (beforeRelease) check(snapshot().size() == 4, "a new overlap cancels the scheduled release", mode);
    else std::printf("[%s] a new overlap: skipped, the machine overslept\n", mode);

    // A committed barge-in releases as soon as the interrupt resolves, without waiting.
    const size_t beforeInterrupt = snapshot().size();
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT));
    rig.send(agent(transport::AgentStateTransition::INTERRUPT_RESOLVED));
    waitUntil([&] { return snapshot().size() > beforeInterrupt; }, 1000ms);
    events = snapshot();
    check(events.size() == beforeInterrupt + 1 && !events.back().first, "interrupt resolution releases at once", mode);
    const size_t afterInterrupt = events.size();
    std::this_thread::sleep_for(400ms);
    check(snapshot().size() == afterInterrupt, "no stray release later", mode);
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

// The user resumed turn 3: what's queued of its reply goes, and other speech stays.
void replacedTurnQueued() {
    const char* mode = "replaced turn, queued";
    Rig rig(false, false);   // one sentence at a time: the reply's waits in the queue
    rig.send(text("Intro. ", false));          // not a reply: never dropped
    rig.send(reply("Got it. ", false, 3));
    rig.send(reply("Anything else?", false, 3));
    rig.send(replaced(3));                     // its final never comes
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; });
    check(rig.provider->received == std::vector<std::string>{"Intro. "}, "the replaced turn's sentences dropped", mode);
    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::COMPLETED},
          "the turn still completes", mode);
}

// Turn 3's reply is playing: it stops, the turn completes (no INTERRUPTED: the user didn't cut in),
// its late text is dropped, and turn 4's reply is a clean new turn.
void replacedTurnPlaying() {
    const char* mode = "replaced turn, playing";
    Rig rig(true, false);
    rig.send(reply("Alpha. ", false, 3));
    std::this_thread::sleep_for(30ms);
    rig.send(replaced(3));
    rig.send(reply("Beta.", true, 3));          // the stale reply's rest, arriving after
    waitUntil([&] { return rig.recorder->snapshot().size() >= 2; });
    rig.send(reply("Gamma.", true, 4));
    waitUntil([&] { return rig.recorder->snapshot().size() >= 4; });
    check(rig.provider->received == std::vector<std::string>{"Alpha. ", "Gamma."}, "late text of the replaced turn dropped", mode);
    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED, SynthesisState::COMPLETED,
                                                                 SynthesisState::STARTED, SynthesisState::COMPLETED},
          "stopped turn completes, then a clean new one", mode);
}

} // namespace

int main() {
    streamedReply(true);
    streamedReply(false);
    bargeIn(false);
    bargeIn(true);
    pausedReply();
    duckLastsMinimumDuration();
    protectedPart(true);
    protectedPart(false);
    replacedTurnQueued();
    replacedTurnPlaying();
    std::printf("tts turn scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
