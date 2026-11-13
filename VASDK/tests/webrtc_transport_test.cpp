// WebRtcTransport against a fake WebRtcInterface: peer audio becomes ring-buffer-backed AudioFrames
// covering exactly the samples received; synthesis audio leaves in paced 20 ms chunks padded with
// silence; flushOutputAudio() drops all but a faded tail; connect/disconnect drive the state.

#include <VASDK/WebRtcTransport.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using va::test::waitUntil;

namespace {

using Chunks = std::vector<std::vector<int16_t>>;

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

    Chunks sentChunks() {
        std::lock_guard<std::mutex> l(m);
        return chunks;
    }
    std::vector<std::string> sentEvents() {
        std::lock_guard<std::mutex> l(m);
        return events;
    }
    void clearChunks() {
        std::lock_guard<std::mutex> l(m);
        chunks.clear();
    }

    int inRate = 0, outRate = 0;
    std::atomic<bool> connected{false};
    std::mutex m;
    Chunks chunks;
    std::vector<std::string> events;
};

std::shared_ptr<transport::SynthesisFrame> synth(size_t samples, int16_t value) {
    auto f = std::make_shared<transport::SynthesisFrame>();
    std::vector<int16_t> pcm(samples, value);
    f->audioData.assign(reinterpret_cast<char*>(pcm.data()),
                        reinterpret_cast<char*>(pcm.data() + pcm.size()));
    return f;
}

// Index of the first chunk that starts with `value`, or the chunk count.
size_t firstChunkStartingWith(const Chunks& chunks, int16_t value) {
    size_t index = 0;
    while (index < chunks.size() && chunks[index][0] != value) ++index;
    return index;
}

// A transport with 320-sample frames, started, its peer connected.
class WebRtcTransportTest : public testing::Test {
protected:
    void SetUp() override {
        webRtc.setFrameSize(320);
        webRtc.setRingBuffer(ring);
        ASSERT_TRUE(webRtc.initialize()) << "initialize";
        ASSERT_TRUE(webRtc.start()) << "start";
        webRtc.onConnected = [this] { ++connects; };
        webRtc.onDisconnected = [this] { ++disconnects; };
        peer->connect();
    }
    void TearDown() override {
        webRtc.stop();
        webRtc.release();
    }

    std::shared_ptr<FakePeer> peer = std::make_shared<FakePeer>();
    std::shared_ptr<AudioRingBuffer> ring = std::make_shared<AudioRingBuffer>(16000 * 10, 2);
    transport::WebRtcTransport webRtc{peer};
    std::atomic<int> connects{0}, disconnects{0};
};

TEST(WebRtcTransport, IsConnectingUntilAPeerConnects) {
    auto peer = std::make_shared<FakePeer>();
    transport::WebRtcTransport webRtc(peer);
    webRtc.setFrameSize(320);
    webRtc.setRingBuffer(std::make_shared<AudioRingBuffer>(16000 * 10, 2));
    ASSERT_TRUE(webRtc.initialize() && webRtc.start()) << "initialize/start";
    EXPECT_EQ(peer->inRate, 16000) << "open() gets the configured input rate";
    EXPECT_EQ(peer->outRate, 16000) << "open() gets the configured output rate";
    EXPECT_EQ(webRtc.getState(), transport::TransportState::CONNECTING) << "CONNECTING before a peer";
    EXPECT_FALSE(webRtc.sendFrame(synth(320, 1))) << "synthesis refused without a peer";
    int connects = 0;
    webRtc.onConnected = [&] { ++connects; };
    peer->connect();
    EXPECT_TRUE(webRtc.isConnected()) << "peer connect -> CONNECTED";
    EXPECT_EQ(connects, 1) << "peer connect -> onConnected";
    webRtc.stop();
    webRtc.release();
    EXPECT_EQ(webRtc.getState(), transport::TransportState::DISCONNECTED) << "stopped -> DISCONNECTED";
}

// 480 + 480 samples in odd-sized pushes -> three 320-sample frames, no leftovers lost.
TEST_F(WebRtcTransportTest, FramesPeerAudioInContiguousSpans) {
    std::vector<int16_t> in(480);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);
    peer->onAudioReceived(in.data(), in.size());
    peer->onAudioReceived(in.data(), in.size());
    std::vector<std::shared_ptr<transport::AudioFrame>> frames;
    while (auto f = webRtc.receiveFrame(0)) frames.push_back(std::dynamic_pointer_cast<transport::AudioFrame>(f));
    ASSERT_EQ(frames.size(), 3u) << "960 samples -> three 320-sample frames";
    for (size_t i = 0; i < frames.size(); ++i) {
        ASSERT_NE(frames[i], nullptr) << "frame " << i << " is an AudioFrame";
        EXPECT_EQ(frames[i]->numSamples, 320u) << "frame " << i << " is 320 samples";
        EXPECT_EQ(frames[i]->absoluteOffset, i * 320) << "frame " << i << " follows the one before";
    }
    EXPECT_EQ(ring->getOffset(), 960u) << "ring buffer holds exactly the framed samples";
}

// 500 samples -> a 320-sample chunk, then 180 real + 140 silence, then pure silence.
TEST_F(WebRtcTransportTest, PacesSynthesisIn20MsChunks) {
    peer->clearChunks();
    const auto sent = std::chrono::steady_clock::now();
    webRtc.sendFrame(synth(500, 1000));
    // Speech, its padded tail and one idle chunk: three ticks once speech starts.
    waitUntil([&] {
        const auto chunks = peer->sentChunks();
        return firstChunkStartingWith(chunks, 1000) + 2 < chunks.size();
    });
    const auto chunks = peer->sentChunks();
    // One chunk per 20 ms of the time that really passed (a loaded machine sleeps longer than asked),
    // plus two of slack: a burst would send them all at once
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent);
    for (const auto& chunk : chunks) EXPECT_EQ(chunk.size(), 320u) << "paced sender emits 320-sample (20 ms) chunks";
    EXPECT_LE(chunks.size(), static_cast<size_t>(elapsed.count() / 20 + 2)) << "sender is paced, not bursting";
    // A silence tick may land between clearChunks() and sendFrame(); find where speech starts.
    const size_t speech = firstChunkStartingWith(chunks, 1000);
    EXPECT_LE(speech, 1u) << "speech starts on the next tick";
    ASSERT_LT(speech + 2, chunks.size()) << "speech, its tail and an idle chunk were sent";
    EXPECT_EQ(chunks[speech][319], 1000) << "first chunk is all speech";
    EXPECT_EQ(chunks[speech + 1][179], 1000) << "the tail's speech";
    EXPECT_EQ(chunks[speech + 1][180], 0) << "tail padded with silence";
    EXPECT_EQ(chunks[speech + 2][0], 0) << "idle sends silence";
    EXPECT_TRUE(waitUntil([&] { return webRtc.getPendingOutputAudioBytes() == 0; })) << "queue drained";
}

// Pending barge-in: Pause sends silence and keeps the queue; release resumes from the same sample.
TEST_F(WebRtcTransportTest, PauseKeepsTheQueueAndSendsSilence) {
    peer->clearChunks();
    webRtc.holdOutputAudio(transport::PendingPlaybackMode::Pause, true, -12.0f);
    webRtc.sendFrame(synth(3200, 1000));  // 200 ms
    // The pause fades the first chunk of speech out; once a chunk has followed it, the queue is held.
    waitUntil([&] {
        const auto chunks = peer->sentChunks();
        return firstChunkStartingWith(chunks, 997) + 1 < chunks.size();  // 1000 at the ramp's first step
    });
    const int pausedPending = webRtc.getPendingOutputAudioBytes();
    std::this_thread::sleep_for(100ms);  // fixed wait: checking nothing is consumed while paused
    EXPECT_GT(pausedPending, 0) << "paused: queue kept";
    EXPECT_EQ(webRtc.getPendingOutputAudioBytes(), pausedPending) << "paused: nothing consumed";
    const auto chunks = peer->sentChunks();
    const size_t ramp = firstChunkStartingWith(chunks, 997);
    ASSERT_LT(ramp + 1, chunks.size()) << "the speech fades out, then a chunk follows";
    EXPECT_EQ(chunks[ramp][319], 0) << "the fade ends in silence";
    for (size_t c = ramp + 1; c < chunks.size(); ++c)
        for (int16_t sample : chunks[c]) ASSERT_EQ(sample, 0) << "paused: silence is sent (chunk " << c << ")";
    webRtc.holdOutputAudio(transport::PendingPlaybackMode::Pause, false, -12.0f);
    EXPECT_TRUE(waitUntil([&] { return webRtc.getPendingOutputAudioBytes() == 0; }))
        << "released: the paused audio plays out";
}

// Duck plays the audio at -12 dB after a 20 ms ramp.
TEST_F(WebRtcTransportTest, DuckPlaysTheAudioQuieter) {
    webRtc.holdOutputAudio(transport::PendingPlaybackMode::Duck, true, -12.0f);
    peer->clearChunks();
    webRtc.sendFrame(synth(3200, 1000));
    EXPECT_TRUE(waitUntil([&] {
        for (const auto& chunk : peer->sentChunks())
            if (chunk[319] > 240 && chunk[319] < 260) return true;  // 1000 * 10^(-12/20) = 251
        return false;
    })) << "ducked chunks play at -12 dB";
    webRtc.holdOutputAudio(transport::PendingPlaybackMode::Duck, false, -12.0f);
}

// Barge-in: a second of audio flushes down to a <= 50 ms faded tail ending in zero.
TEST_F(WebRtcTransportTest, FlushKeepsAShortTail) {
    webRtc.sendFrame(synth(16000, 1000));
    webRtc.flushOutputAudio();
    const int pending = webRtc.getPendingOutputAudioBytes();
    EXPECT_GT(pending, 0) << "flush keeps a tail";
    EXPECT_LE(pending, 800 * 2) << "flush keeps at most a 50 ms tail";
}

// State frames become JSON events for the page; frames it doesn't need are skipped, never failures.
TEST_F(WebRtcTransportTest, SendsStateFramesAsEvents) {
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
    EXPECT_TRUE(webRtc.sendFrame(agent) && webRtc.sendFrame(stt) && webRtc.sendFrame(who)) << "state frames accepted";
    EXPECT_TRUE(webRtc.sendFrame(std::make_shared<transport::ControlFrame>())) << "other frames accepted, not sent";

    const auto events = peer->sentEvents();
    ASSERT_EQ(events.size(), 3u) << "one event per state frame";
    EXPECT_NE(events[0].find("\"type\":\"agent_state\""), std::string::npos) << "agent_state event: " << events[0];
    EXPECT_NE(events[0].find("\"state\":\"THINKING\""), std::string::npos) << "agent_state event: " << events[0];
    EXPECT_NE(events[0].find("\"reason\":\"TRANSCRIPTION_READY\""), std::string::npos) << "agent_state event: " << events[0];
    EXPECT_NE(events[1].find("\"text\":\"say \\\"hi\\\"\""), std::string::npos) << "transcript event, text JSON-escaped: " << events[1];
    EXPECT_NE(events[2].find("\"type\":\"speaker\""), std::string::npos) << "speaker event: " << events[2];
    EXPECT_NE(events[2].find("\"new\":false"), std::string::npos) << "speaker event: " << events[2];
}

// A custom encoder handles a type the transport doesn't know; every event carries delay_ms.
TEST_F(WebRtcTransportTest, SendsACustomEncodersEvents) {
    webRtc.setEventEncoder(transport::FrameType::CONTROL, [](const transport::Frame&) { return std::string("{\"type\":\"x\"}"); });
    webRtc.setEventEncoder(transport::FrameType::METADATA, [](const transport::Frame&) { return std::string(); });
    webRtc.sendFrame(std::make_shared<transport::ControlFrame>());
    webRtc.sendFrame(std::make_shared<transport::MetadataFrame>());
    const auto events = peer->sentEvents();
    ASSERT_EQ(events.size(), 1u) << "the custom encoder's event sent; an empty encoding skipped";
    EXPECT_EQ(events.back().rfind("{\"delay_ms\":", 0), 0u) << "sent with delay_ms: " << events.back();
    EXPECT_NE(events.back().find("\"type\":\"x\"}"), std::string::npos) << "the custom encoding: " << events.back();
}

TEST_F(WebRtcTransportTest, PeerDisconnectDropsQueuedAudio) {
    webRtc.sendFrame(synth(16000, 1000));
    peer->disconnect();
    EXPECT_EQ(webRtc.getState(), transport::TransportState::CONNECTING) << "peer disconnect -> CONNECTING";
    EXPECT_EQ(disconnects.load(), 1) << "peer disconnect -> onDisconnected";
    EXPECT_EQ(webRtc.getPendingOutputAudioBytes(), 0) << "disconnect drops queued audio";
}

// Provider veto on sample rates fails initialize() and leaves no callbacks into the transport.
TEST(WebRtcTransport, ProviderVetoFailsInitialize) {
    auto vetoPeer = std::make_shared<FakePeer>();
    {
        transport::WebRtcTransport bad(vetoPeer);
        audio::AudioConfig c{24000, audio::AudioFormat::SHORT, 1};
        bad.setInputAudioConfig(c);
        EXPECT_FALSE(bad.initialize()) << "initialize fails when provider rejects rates";
    }
    EXPECT_FALSE(vetoPeer->onConnect) << "failed initialize clears provider callbacks";
    EXPECT_FALSE(vetoPeer->onAudioReceived) << "failed initialize clears provider callbacks";
}

TEST(WebRtcTransport, RejectsFrameSizeZero) {
    transport::WebRtcTransport zero(std::make_shared<FakePeer>());
    zero.setFrameSize(0);
    EXPECT_FALSE(zero.initialize()) << "frame size 0 rejected";
}

// A peer already connected when start() runs is picked up.
TEST(WebRtcTransport, PicksUpAPeerConnectedBeforeStart) {
    auto early = std::make_shared<FakePeer>();
    transport::WebRtcTransport late(early);
    late.initialize();
    early->connected = true;
    late.start();
    EXPECT_TRUE(late.isConnected()) << "start() with a connected peer -> CONNECTED";
    late.stop();
    late.release();
}

} // namespace
