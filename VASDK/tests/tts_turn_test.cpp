// TTSFrameProcessor turn model: a reply streamed as several TextFrames yields exactly one
// SynthesisState STARTED and one COMPLETED (after its audio played), for a provider that queues
// calls and for one that must get them one at a time; a barge-in drops the stale reply, even
// when the provider confirms the cancel synchronously.

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

} // namespace

int main() {
    streamedReply(true);
    streamedReply(false);
    bargeIn(false);
    bargeIn(true);
    std::printf("tts turn scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
