// Early end of speech: VAD -> TurnDetection with a provider that calls the turn over as soon as the
// VAD pauses. The VAD must close the segment at once (one ENDED, one user turn END) instead of
// waiting out maxSpeechPauseDurationMs + maxSpeechEndDurationMs; without that call it still does.
// A one-frame noise blip before the speech (VAD STARTING -> QUIET) must not produce a user turn
// START, since that is what barges in on the agent. Confirming speech in STARTING tolerates one
// quiet frame after voice (not two, not a blip), and is quicker while the agent speaks.

#include <VASDK/Pipeline.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/VADFrameProcessor.h>
#include "../src/AudioFrameUtil.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using transport::VADState;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

constexpr int kFrame = 1600;  // 100 ms at 16 kHz

// Voice = any nonzero first sample in the frame; like Silero, reports less than the whole frame as voiced.
class FakeVad : public audio::VADProvider {
public:
    bool initialize(const audio::AudioConfig& c, int) override { _audioConfig = c; return true; }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char* buffer, size_t, size_t n) const override {
        if (reinterpret_cast<const int16_t*>(buffer)[0] == 0) return {};
        return {{0, n * 3 / 4, 0.9f}};
    }
};

// Says "turn over" on the first audio frame seen while the VAD is paused (if enabled).
class FakeTurn : public audio::TurnDetectionProvider {
public:
    explicit FakeTurn(bool early) : _early(early) {}
    TurnEvaluation evaluate(const std::shared_ptr<transport::Frame> frame, bool) override {
        if (auto v = std::dynamic_pointer_cast<transport::VADStateFrame>(frame)) {
            _paused = v->state == VADState::PAUSED || v->state == VADState::ENDING;
            if (v->state == VADState::STARTING) return {Decision::TURN_START, 1.0f};
            if (v->state == VADState::ENDED && !_ended) { _ended = true; return {Decision::TURN_END, 1.0f}; }
        } else if (_early && _paused && !_ended && frame->getType() == transport::FrameType::AUDIO) {
            _ended = true;
            return {Decision::TURN_END, 0.99f};
        }
        return {};
    }
private:
    bool _early, _paused = false, _ended = false;
};

class Source : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Source"; }
    void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection d) override {
        if (d == pipeline::FlowDirection::DOWNSTREAM) pushDownstream(f);
    }
    void send(std::shared_ptr<transport::Frame> f) { pushDownstream(f); }
};

class Sink : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Sink"; }
    void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection) override {
        std::lock_guard<std::mutex> l(m);
        if (auto v = std::dynamic_pointer_cast<transport::VADStateFrame>(f)) {
            if (v->state == VADState::ENDED) { ++ended; segment = v->totalSpeechLength; }
        } else if (auto t = std::dynamic_pointer_cast<transport::TurnStateFrame>(f)) {
            if (t->actor == transport::TurnStateFrame::TurnActor::USER) {
                if (t->state == transport::TurnStateFrame::TurnState::END) ++userEnds;
                else ++userStarts;
            }
        }
    }
    std::mutex m;
    int ended = 0, userEnds = 0, userStarts = 0;
    size_t segment = 0;
};

/// Feeds 1 s of speech then silence; returns how many silent frames it took to see ENDED.
int run(bool early, Sink*& sinkOut, std::shared_ptr<pipeline::Pipeline>& keep) {
    audio::AudioConfig cfg{16000, audio::AudioFormat::SHORT, 1};
    auto ring = std::make_shared<AudioRingBuffer>(16000 * 10, 2);
    auto vadProvider = std::make_shared<FakeVad>();
    vadProvider->initialize(cfg, kFrame);
    auto vad = std::make_shared<pipeline::VADFrameProcessor>(vadProvider);
    pipeline::VADConfig vc;
    vc.minSpeechDurationMs = 250;  // the apps' value: must still reach ONGOING with 100 ms frames
    vc.maxSpeechPauseDurationMs = 500;
    vc.maxSpeechEndDurationMs = 250;
    vad->setConfig(vc);
    vad->setRingBuffer(ring);
    auto source = std::make_shared<Source>();
    auto sink = std::make_shared<Sink>();
    keep = std::make_shared<pipeline::Pipeline>();
    keep->addProcessor(source).addProcessor(vad)
        .addProcessor(std::make_shared<pipeline::TurnDetectionProcessor>(std::make_shared<FakeTurn>(early)))
        .addProcessor(sink);
    if (!keep->build() || !keep->start()) { check(false, "pipeline starts"); return -1; }
    sinkOut = sink.get();

    uint64_t abs = 0, seq = 0;
    std::vector<int16_t> speech(kFrame, 1000), silence(kFrame, 0);
    auto feed = [&](const std::vector<int16_t>& pcm) {
        source->send(transport::detail::writeAudioFrame(ring.get(), reinterpret_cast<const char*>(pcm.data()),
                                                        kFrame, cfg, abs, seq++, "test"));
        std::this_thread::sleep_for(40ms);  // let the frame and any upstream request settle
    };
    feed(speech);                                // blip
    for (int i = 0; i < 5; ++i) feed(silence);
    for (int i = 0; i < 10; ++i) feed(speech);
    for (int i = 1; i <= 15; ++i) {
        feed(silence);
        std::lock_guard<std::mutex> l(sink->m);
        if (sink->ended) return i;
    }
    return 99;
}

/// Feeds `pattern` ('V' voice, '.' quiet) to a VAD alone; returns the 1-based frame at which it
/// reached ONGOING, or 0 if it never did.
int framesToOngoing(const std::string& pattern, bool agentSpeaking) {
    audio::AudioConfig cfg{16000, audio::AudioFormat::SHORT, 1};
    auto ring = std::make_shared<AudioRingBuffer>(16000 * 10, 2);
    auto vadProvider = std::make_shared<FakeVad>();
    vadProvider->initialize(cfg, kFrame);
    auto vad = std::make_shared<pipeline::VADFrameProcessor>(vadProvider);
    pipeline::VADConfig vc;
    vc.minSpeechDurationMs = 250;
    vc.bargeInMinSpeechDurationMs = 150;
    vad->setConfig(vc);
    vad->setRingBuffer(ring);
    vad->setAgentSpeaking([agentSpeaking] { return agentSpeaking; });
    auto source = std::make_shared<Source>();
    struct States : pipeline::FrameProcessor {
        std::string getName() const override { return "States"; }
        void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection) override {
            if (auto v = std::dynamic_pointer_cast<transport::VADStateFrame>(f); v && v->state == VADState::ONGOING) ongoing = true;
        }
        std::atomic<bool> ongoing{false};
    };
    auto states = std::make_shared<States>();
    pipeline::Pipeline pipeline;
    pipeline.addProcessor(source).addProcessor(vad).addProcessor(states);
    if (!pipeline.build() || !pipeline.start()) return -1;
    uint64_t abs = 0, seq = 0;
    std::vector<int16_t> speech(kFrame, 1000), silence(kFrame, 0);
    int reached = 0;
    for (size_t i = 0; i < pattern.size() && !reached; ++i) {
        const auto& pcm = pattern[i] == 'V' ? speech : silence;
        source->send(transport::detail::writeAudioFrame(ring.get(), reinterpret_cast<const char*>(pcm.data()),
                                                        kFrame, cfg, abs, seq++, "test"));
        std::this_thread::sleep_for(30ms);
        if (states->ongoing) reached = static_cast<int>(i + 1);
    }
    pipeline.stop();
    return reached;
}

} // namespace

int main() {
    // Confirming speech: decided 250 ms in (the 4th 100 ms frame), or 150 ms in while the agent speaks.
    check(framesToOngoing("VVVVV", false) == 4, "steady speech confirmed on the 4th frame");
    check(framesToOngoing("VVV.VV", false) == 5, "one quiet frame at the decision is tolerated");
    check(framesToOngoing("VVV..VV", false) == 0, "two quiet frames in a row are not");
    check(framesToOngoing("V.....", false) == 0, "a lone blip is not speech");
    check(framesToOngoing("VVVVV", true) == 3, "a barge-in is confirmed sooner while the agent speaks");
    check(framesToOngoing("VV.VV", true) == 4, "with the same one-frame tolerance");

    {
        Sink* sink = nullptr;
        std::shared_ptr<pipeline::Pipeline> p;
        int n = run(true, sink, p);
        std::this_thread::sleep_for(200ms);
        {
            std::lock_guard<std::mutex> l(sink->m);
            check(n <= 2, "confident turn end closes the segment at the pause");
            check(sink->ended == 1 && sink->userEnds == 1, "exactly one ENDED and one user turn END");
            check(sink->userStarts == 1, "a noise blip before the speech starts no user turn");
            check(sink->segment >= 10 * kFrame, "segment covers the speech");
        }
        std::printf("early: ENDED after %d silent frame(s)\n", n);
        p->stop();
    }
    {
        Sink* sink = nullptr;
        std::shared_ptr<pipeline::Pipeline> p;
        int n = run(false, sink, p);
        check(n >= 7 && n < 99, "without a turn-end call the pause/end timers still apply");
        std::printf("timers: ENDED after %d silent frame(s)\n", n);
        p->stop();
    }
    std::printf("vad turn end checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
