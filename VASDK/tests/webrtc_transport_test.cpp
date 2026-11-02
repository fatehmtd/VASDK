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
    t.sendFrame(synth(500, 1000));
    std::this_thread::sleep_for(130ms);
    {
        std::lock_guard<std::mutex> l(peer->m);
        auto& ch = peer->chunks;
        bool sized = !ch.empty();
        for (auto& c : ch) sized &= c.size() == 320;
        check(sized, "paced sender emits 320-sample (20 ms) chunks");
        check(ch.size() <= 8, "sender is paced, not bursting");
        // A silence tick may land between clear() and sendFrame(); find where speech starts.
        size_t k = 0;
        while (k < ch.size() && ch[k][0] != 1000) ++k;
        check(k <= 1 && k + 2 < ch.size(), "speech starts on the next tick");
        if (k + 2 < ch.size()) {
            check(ch[k][319] == 1000, "first chunk is all speech");
            check(ch[k + 1][179] == 1000 && ch[k + 1][180] == 0, "tail padded with silence");
            check(ch[k + 2][0] == 0, "idle sends silence");
        }
    }
    check(t.getPendingOutputAudioBytes() == 0, "queue drained");

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
