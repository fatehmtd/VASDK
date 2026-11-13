// WebRtcTransport against a fake WebRtcInterface: peer audio becomes ring-buffer-backed AudioFrames
// covering exactly the samples received; synthesis audio leaves in paced 20 ms chunks padded with
// silence; flushOutputAudio() drops all but a faded tail; connect/disconnect drive the state.

#include <VASDK/WebRtcTransport.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;

namespace {

class FakePeer : public network::WebRtcInterface {
public:
    bool open(int in, int out) override { inRate = in; outRate = out; return in == 16000; }
    void close() override { connected = false; }
    bool isConnected() const override { return connected; }
    bool sendAudio(const int16_t* pcm, size_t samples) override {
        std::lock_guard<std::mutex> l(m);
        chunks.emplace_back(pcm, pcm + samples);
        return true;
    }

    bool sendEvent(const std::string& json) override {
        std::lock_guard<std::mutex> l(m);
        events.push_back(json);
        return true;
    }

    void connect() { connected = true; onConnect(); }
    void disconnect() { connected = false; onDisconnect(); }

    int inRate = 0, outRate = 0;
    std::atomic<bool> connected{false};
    std::mutex m;
    std::vector<std::vector<int16_t>> chunks;
    std::vector<std::string> events;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

// Until `condition` holds, at most `timeout`. A check that something happens waits for it, not a
// fixed time: a loaded machine (a shared CI runner) runs the sender late. A check that something
// doesn't happen still waits a fixed time.
template <class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// Index of the first chunk that starts with `value`, or the chunk count.
size_t firstChunkStartingWith(const std::vector<std::vector<int16_t>>& chunks, int16_t value) {
    size_t index = 0;
    while (index < chunks.size() && chunks[index][0] != value) ++index;
    return index;
}

std::shared_ptr<transport::SynthesisFrame> synth(size_t samples, int16_t value) {
    auto f = std::make_shared<transport::SynthesisFrame>();
    std::vector<int16_t> pcm(samples, value);
    f->audioData.assign(reinterpret_cast<char*>(pcm.data()),
                        reinterpret_cast<char*>(pcm.data() + pcm.size()));
    return f;
}

} // namespace

int main() {
    auto peer = std::make_shared<FakePeer>();
    auto ring = std::make_shared<AudioRingBuffer>(16000 * 10, 2);
    transport::WebRtcTransport t(peer);
    t.setFrameSize(320);
    t.setRingBuffer(ring);

    check(t.initialize() && t.start(), "initialize/start");
    check(peer->inRate == 16000 && peer->outRate == 16000, "open() gets configured rates");
    check(t.getState() == transport::TransportState::CONNECTING, "CONNECTING before a peer");
    check(!t.sendFrame(synth(320, 1)), "synthesis refused without a peer");

    int connects = 0, disconnects = 0;
    t.onConnected = [&] { ++connects; };
    t.onDisconnected = [&] { ++disconnects; };
    peer->connect();
    check(t.isConnected() && connects == 1, "peer connect -> CONNECTED + onConnected");

    // Inbound: 480 + 480 samples in odd-sized pushes -> three 320-sample frames, no leftovers lost.
    std::vector<int16_t> in(480);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);
    peer->onAudioReceived(in.data(), in.size());
    peer->onAudioReceived(in.data(), in.size());
    std::vector<std::shared_ptr<transport::AudioFrame>> frames;
    while (auto f = t.receiveFrame(0)) frames.push_back(std::dynamic_pointer_cast<transport::AudioFrame>(f));
    check(frames.size() == 3, "960 samples -> three 320-sample frames");
    bool contiguous = true;
    for (size_t i = 0; i < frames.size(); ++i)
        contiguous &= frames[i] && frames[i]->numSamples == 320 && frames[i]->absoluteOffset == i * 320;
    check(contiguous, "frames are contiguous 320-sample spans");
    check(ring->getOffset() == 960, "ring buffer holds exactly the framed samples");

    // Outbound: 500 samples -> a 320-sample chunk, then 180 real + 140 silence, then pure silence.
    { std::lock_guard<std::mutex> l(peer->m); peer->chunks.clear(); }
    const auto sent = std::chrono::steady_clock::now();
    t.sendFrame(synth(500, 1000));
    // Speech, its padded tail and one idle chunk: three ticks once speech starts.
    waitUntil([&] {
        std::lock_guard<std::mutex> l(peer->m);
        return firstChunkStartingWith(peer->chunks, 1000) + 2 < peer->chunks.size();
    });
    {
        std::lock_guard<std::mutex> l(peer->m);
        // One chunk per 20 ms of the time that really passed (a loaded machine sleeps longer than asked),
        // plus two of slack: a burst would send them all at once
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent);
        auto& ch = peer->chunks;
        bool sized = !ch.empty();
        for (auto& c : ch) sized &= c.size() == 320;
        check(sized, "paced sender emits 320-sample (20 ms) chunks");
        check(ch.size() <= static_cast<size_t>(elapsed.count() / 20 + 2), "sender is paced, not bursting");
        // A silence tick may land between clear() and sendFrame(); find where speech starts.
        const size_t k = firstChunkStartingWith(ch, 1000);
        check(k <= 1 && k + 2 < ch.size(), "speech starts on the next tick");
        if (k + 2 < ch.size()) {
            check(ch[k][319] == 1000, "first chunk is all speech");
            check(ch[k + 1][179] == 1000 && ch[k + 1][180] == 0, "tail padded with silence");
            check(ch[k + 2][0] == 0, "idle sends silence");
        }
    }
    check(waitUntil([&] { return t.getPendingOutputAudioBytes() == 0; }), "queue drained");

    // Pending barge-in: Pause sends silence and keeps the queue; release resumes from the same
    // sample. Duck plays the audio at -12 dB after a 20 ms ramp.
    { std::lock_guard<std::mutex> l(peer->m); peer->chunks.clear(); }
    t.holdOutputAudio(transport::PendingPlaybackMode::Pause, true, -12.0f);
    t.sendFrame(synth(3200, 1000));  // 200 ms
    // The pause fades the first chunk of speech out; once a chunk has followed it, the queue is held.
    waitUntil([&] {
        std::lock_guard<std::mutex> l(peer->m);
        return firstChunkStartingWith(peer->chunks, 997) + 1 < peer->chunks.size();  // 1000 at the ramp's first step
    });
    const int pausedPending = t.getPendingOutputAudioBytes();
    std::this_thread::sleep_for(100ms);
    check(pausedPending > 0 && t.getPendingOutputAudioBytes() == pausedPending, "paused: queue kept, nothing consumed");
    {
        std::lock_guard<std::mutex> l(peer->m);
        auto& ch = peer->chunks;
        const size_t ramp = firstChunkStartingWith(ch, 997);
        bool silent = ramp + 1 < ch.size() && ch[ramp][319] == 0;
        for (size_t c = ramp + 1; c < ch.size(); ++c)
            for (int16_t sample : ch[c]) silent &= sample == 0;
        check(silent, "paused: the speech fades out, then silence is sent");
        ch.clear();
    }
    t.holdOutputAudio(transport::PendingPlaybackMode::Pause, false, -12.0f);
    check(waitUntil([&] { return t.getPendingOutputAudioBytes() == 0; }), "released: the paused audio plays out");
    t.holdOutputAudio(transport::PendingPlaybackMode::Duck, true, -12.0f);
    { std::lock_guard<std::mutex> l(peer->m); peer->chunks.clear(); }
    t.sendFrame(synth(3200, 1000));
    check(waitUntil([&] {
              std::lock_guard<std::mutex> l(peer->m);
              for (auto& c : peer->chunks)
                  if (c[319] > 240 && c[319] < 260) return true;  // 1000 * 10^(-12/20) = 251
              return false;
          }),
          "ducked chunks play at -12 dB");
    t.holdOutputAudio(transport::PendingPlaybackMode::Duck, false, -12.0f);
    waitUntil([&] { return t.getPendingOutputAudioBytes() == 0; });

    // Barge-in: a second of audio flushes down to a <= 50 ms faded tail ending in zero.
    t.sendFrame(synth(16000, 1000));
    t.flushOutputAudio();
    int pending = t.getPendingOutputAudioBytes();
    check(pending > 0 && pending <= 800 * 2, "flush keeps at most a 50 ms tail");

    // State frames become JSON events for the page; frames it doesn't need are skipped, never failures.
    auto agent = std::make_shared<transport::AgentStateFrame>();
    agent->previousState = transport::AgentState::LISTENING;
    agent->currentState = transport::AgentState::THINKING;
    agent->transition = transport::AgentStateTransition::TRANSCRIPTION_READY;
    auto stt = std::make_shared<transport::TranscriptionFrame>();
    stt->isFinal = true;
    stt->transcriptionResult.text = "say \"hi\"";
    auto who = std::make_shared<transport::SpeakerIdentificationFrame>();
    who->isNewSpeaker = false;
    who->trackedSimilarity = 0.5f;
    check(t.sendFrame(agent) && t.sendFrame(stt) && t.sendFrame(who), "state frames accepted");
    check(t.sendFrame(std::make_shared<transport::ControlFrame>()), "other frames accepted, not sent");
    {
        std::lock_guard<std::mutex> l(peer->m);
        check(peer->events.size() == 3, "one event per state frame");
        if (peer->events.size() == 3) {
            auto& a = peer->events[0];
            check(a.find("\"type\":\"agent_state\"") != std::string::npos &&
                  a.find("\"state\":\"THINKING\"") != std::string::npos &&
                  a.find("\"reason\":\"TRANSCRIPTION_READY\"") != std::string::npos, "agent_state event");
            check(peer->events[1].find("\"text\":\"say \\\"hi\\\"\"") != std::string::npos,
                  "transcript event, text JSON-escaped");
            check(peer->events[2].find("\"type\":\"speaker\"") != std::string::npos &&
                  peer->events[2].find("\"new\":false") != std::string::npos, "speaker event");
        }
    }

    // A custom encoder handles a type the transport doesn't know; every event carries delay_ms.
    t.setEventEncoder(transport::FrameType::CONTROL, [](const transport::Frame&) { return std::string("{\"type\":\"x\"}"); });
    t.setEventEncoder(transport::FrameType::METADATA, [](const transport::Frame&) { return std::string(); });
    t.sendFrame(std::make_shared<transport::ControlFrame>());
    t.sendFrame(std::make_shared<transport::MetadataFrame>());
    {
        std::lock_guard<std::mutex> l(peer->m);
        check(peer->events.size() == 4 && peer->events.back().rfind("{\"delay_ms\":", 0) == 0 &&
              peer->events.back().find("\"type\":\"x\"}") != std::string::npos,
              "custom encoder event sent with delay_ms; empty encoding skipped");
    }

    peer->disconnect();
    check(t.getState() == transport::TransportState::CONNECTING && disconnects == 1,
          "peer disconnect -> CONNECTING + onDisconnected");
    check(t.getPendingOutputAudioBytes() == 0, "disconnect drops queued audio");

    t.stop();
    t.release();
    check(t.getState() == transport::TransportState::DISCONNECTED, "stopped -> DISCONNECTED");

    // Provider veto on sample rates fails initialize() and leaves no callbacks into the transport.
    auto vetoPeer = std::make_shared<FakePeer>();
    {
        transport::WebRtcTransport bad(vetoPeer);
        audio::AudioConfig c{24000, audio::AudioFormat::SHORT, 1};
        bad.setInputAudioConfig(c);
        check(!bad.initialize(), "initialize fails when provider rejects rates");
    }
    check(!vetoPeer->onConnect && !vetoPeer->onAudioReceived, "failed initialize clears provider callbacks");

    transport::WebRtcTransport zero(std::make_shared<FakePeer>());
    zero.setFrameSize(0);
    check(!zero.initialize(), "frame size 0 rejected");

    // A peer already connected when start() runs is picked up.
    auto early = std::make_shared<FakePeer>();
    transport::WebRtcTransport late(early);
    late.initialize();
    early->connected = true;
    late.start();
    check(late.isConnected(), "start() with a connected peer -> CONNECTED");

    std::printf("webrtc transport checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
