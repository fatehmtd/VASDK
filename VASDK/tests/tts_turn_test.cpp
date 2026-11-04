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
    std::this_thread::sleep_for(700ms);

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
    rig.send(text("Alpha. ", false));
    std::this_thread::sleep_for(50ms);
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT));
    rig.send(text("Beta.", true));  // stale sentence of the interrupted reply
    std::this_thread::sleep_for(100ms);
    rig.send(agent(transport::AgentStateTransition::INTERRUPT_RESOLVED));
    rig.send(text("Gamma.", true));
    std::this_thread::sleep_for(500ms);

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
    std::this_thread::sleep_until(t0 + 60ms);
    check(rig.tts->getProtectedPlaybackMs() == 0, "not during the part before", mode);
    std::this_thread::sleep_until(t0 + 170ms);
    check(rig.tts->getProtectedPlaybackMs() > 0, "while it plays", mode);
    std::this_thread::sleep_until(t0 + 300ms);
    check(rig.tts->getProtectedPlaybackMs() == 0, "not during the part after", mode);
    std::this_thread::sleep_for(300ms);
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
    std::this_thread::sleep_for(60ms);
    check(rig.tts->getRemainingReplyPlaybackMs() >= 0, "reply text complete: remaining playback known", mode);
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    std::this_thread::sleep_for(400ms);  // longer than the 200 ms reply: only the pause keeps it going
    check(rig.recorder->snapshot() == std::vector<SynthesisState>{SynthesisState::STARTED}, "no COMPLETED while paused", mode);
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    std::this_thread::sleep_for(500ms);

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
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    std::this_thread::sleep_for(50ms);
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    std::this_thread::sleep_for(150ms);
    check(snapshot().size() == 1, "still ducked before the minimum duration", mode);
    std::this_thread::sleep_for(250ms);
    auto events = snapshot();
    check(events.size() == 2 && !events[1].first, "released once the minimum duration passed", mode);
    if (events.size() == 2) {
        const auto heldFor = std::chrono::duration_cast<std::chrono::milliseconds>(events[1].second - events[0].second);
        check(heldFor >= 290ms && heldFor < 400ms, "held for about the minimum duration", mode);
    }

    // Speech over the agent again while the release is pending: stays ducked from the new start.
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    rig.send(agent(transport::AgentStateTransition::BACKCHANNEL));
    std::this_thread::sleep_for(200ms);
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_PENDING));
    std::this_thread::sleep_for(200ms);
    check(snapshot().size() == 4, "a new overlap cancels the scheduled release", mode);

    // A committed barge-in releases as soon as the interrupt resolves, without waiting.
    rig.send(agent(transport::AgentStateTransition::BARGE_IN_INTERRUPT));
    rig.send(agent(transport::AgentStateTransition::INTERRUPT_RESOLVED));
    std::this_thread::sleep_for(30ms);
    events = snapshot();
    check(events.size() == 5 && !events[4].first, "interrupt resolution releases at once", mode);
    std::this_thread::sleep_for(400ms);
    check(snapshot().size() == 5, "no stray release later", mode);
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
    std::printf("tts turn scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
