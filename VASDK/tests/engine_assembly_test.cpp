// VAEngine assembly: providers come from the registry by id, the standard chain is built in
// order with app processors at their slots, the LLM stages appear only with an `llm` endpoint,
// a missing provider fails create() with its kind, say() text reaches the TTS provider, and the
// config's audio formats, voice activity timings and back-channel values reach the pipeline.

#include "engine_test_fakes.h"

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/Pipeline.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/VAEngine.h>
#include <VASDK/network/WebRtcInterface.h>

#include <atomic>
#include <chrono>
#include <cstdio>

using namespace va;
using namespace fakes;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool before(const std::string& text, const std::string& first, const std::string& second) {
    return contains(text, first) && contains(text, second) && text.find(first) < text.find(second);
}

// A WebRTC peer that only says whether it was opened.
class FakePeer : public network::WebRtcInterface {
public:
    bool open(int input, int output) override { opened = input == 16000 && output == 16000; return opened; }
    void close() override {}
    bool isConnected() const override { return false; }
    bool sendAudio(const int16_t*, size_t) override { return false; }
    std::atomic<bool> opened{false};
};

} // namespace

int main() {
    auto tts = std::make_shared<FakeTts>();
    const auto registry = makeRegistry(tts);

    // Without an LLM: the standard chain, app processors at their slots, no LLM stages.
    vasdk::engine::EngineConfig offline;
    auto transport = std::make_shared<FakeTransport>();
    VAEngine::Options options;
    options.transport = transport;
    std::string error;
    auto engine = VAEngine::create(offline, registry, options, &error);
    check(engine != nullptr, "offline engine created");
    if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
    check(transport->ringBuffer == engine->inputRingBuffer() && transport->frameSize == 1600,
          "transport configured with the engine's ring buffer and frame size");
    check(engine->addProcessor(VAEngine::Slot::AfterInput, std::make_shared<Named>("MicRecorder")), "slot accepted");
    engine->addProcessor(VAEngine::Slot::BeforeOutput, std::make_shared<Named>("Visualiser"));
    const std::string offlineChain = engine->describe();
    check(!engine->hasLLM() && !contains(offlineChain, "LLMProcessor"), "no LLM stages without an llm endpoint");
    check(before(offlineChain, "TransportInputProcessor", "MicRecorder") &&
          before(offlineChain, "MicRecorder", "VADFrameProcessor (FakeVad)") &&
          before(offlineChain, "VADFrameProcessor", "TranscriptionFrameProcessor (FakeStt)") &&
          before(offlineChain, "AgentStateProcessor", "TTSFrameProcessor (FakeTts)") &&
          before(offlineChain, "TTSFrameProcessor", "Visualiser") &&
          before(offlineChain, "Visualiser", "TransportOutputProcessor"),
          "standard order with slots");
    if (failures) std::fprintf(stderr, "  chain: %s\n", offlineChain.c_str());

    // say() reaches the TTS provider through the running pipeline.
    check(engine->start(&error), "engine starts");
    check(!engine->addProcessor(VAEngine::Slot::AfterInput, std::make_shared<Named>("Late")), "no slots after start");
    engine->say("Hello there.");
    bool spoke = false;
    for (int i = 0; i < 100 && !spoke; ++i) {
        for (const auto& text : tts->said()) spoke = spoke || contains(text, "Hello there.");
        if (!spoke) std::this_thread::sleep_for(20ms);
    }
    check(spoke, "say() text synthesized");
    engine->interrupt();   // must not crash with nothing playing
    engine->stop();
    engine.reset();

    // With an llm endpoint: LLM and tool stages between the agent state and TTS.
    vasdk::engine::EngineConfig online;
    online.mutable_llm()->set_endpoint("http://127.0.0.1:9/v1/chat/completions");
    online.mutable_llm()->set_model("test");
    options.transport = std::make_shared<FakeTransport>();
    auto onlineEngine = VAEngine::create(online, registry, options, &error);
    check(onlineEngine && onlineEngine->hasLLM(), "LLM enabled by an llm endpoint");
    if (onlineEngine) {
        const std::string chain = onlineEngine->describe();
        check(before(chain, "AgentStateProcessor", "LLMProcessor") && before(chain, "LLMProcessor", "ToolDispatchProcessor") &&
              before(chain, "ToolDispatchProcessor", "TTSFrameProcessor"), "LLM stages in order");
    }

    // A provider that isn't registered fails create() and names its kind.
    vasdk::engine::EngineConfig missing;
    missing.mutable_providers()->mutable_stt()->set_id("parakeet");
    check(!VAEngine::create(missing, registry, options, &error) && contains(error, "STT: ") &&
          contains(error, "parakeet"), "missing provider reported with its kind");
    check(!VAEngine::create(offline, registry, VAEngine::Options{}, &error) && contains(error, "transport"),
          "transport is required");

    // Audio formats, voice activity and back-channel values from the config replace the defaults one by one.
    vasdk::engine::EngineConfig tuned;
    tuned.mutable_turn_taking()->set_preset(vasdk::engine::TurnTaking::SNAPPY);
    tuned.mutable_turn_taking()->mutable_voice_activity()->set_end_silence_ms(900);
    auto* backchannel = tuned.mutable_turn_taking()->mutable_backchannel();
    backchannel->set_pending_playback(vasdk::engine::Backchannel::PAUSE);
    backchannel->set_commit_after_ms(800);
    backchannel->add_interrupt_words("halt");
    tuned.mutable_turn_taking()->set_interrupt_timeout_ms(700);
    auto* audioConfig = tuned.mutable_audio();
    audioConfig->mutable_input()->set_sample_rate(24000);
    audioConfig->mutable_output()->set_sample_rate(48000);
    audioConfig->mutable_output()->set_encoding(vasdk::engine::AudioFormat::FLOAT_32);
    audioConfig->set_frame_ms(50);
    auto tunedTransport = std::make_shared<FakeTransport>();
    options.transport = tunedTransport;
    auto tunedEngine = VAEngine::create(tuned, registry, options, &error);
    check(tunedEngine && tunedEngine->start(&error), "tuned engine starts");
    if (tunedEngine) {
        check(tunedTransport->inputAudio.sampleRate == 24000 && tunedTransport->inputAudio.format == audio::AudioFormat::SHORT &&
              tunedTransport->inputAudio.channels == 1, "input: the config's rate, the app's encoding and channels");
        check(tunedTransport->outputAudio.sampleRate == 48000 && tunedTransport->outputAudio.format == audio::AudioFormat::FLOAT,
              "output: the config's rate and encoding");
        check(tunedTransport->frameSize == 1200, "frame_ms 50 at 24 kHz");
        std::shared_ptr<pipeline::VADFrameProcessor> vad;
        std::shared_ptr<pipeline::AgentStateProcessor> agentState;
        for (const auto& processor : tunedEngine->pipeline()->getProcessors()) {
            if (!vad) vad = std::dynamic_pointer_cast<pipeline::VADFrameProcessor>(processor);
            if (!agentState) agentState = std::dynamic_pointer_cast<pipeline::AgentStateProcessor>(processor);
        }
        check(vad && vad->getConfig().maxSpeechEndDurationMs == 900 && vad->getConfig().vadThresholdMs == 150,
              "end silence from the config, the rest from SNAPPY");
        if (agentState) {
            const auto& applied = agentState->getBackchannelConfig();
            check(applied.pendingPlaybackMode == transport::PendingPlaybackMode::Pause && applied.commitAfterPendingMs == 800 &&
                  applied.interruptWords == std::vector<std::string>{"halt"} && applied.duckGainDecibels == -12.0f,
                  "back-channel values from the config, the rest from the app");
        } else {
            check(false, "agent state processor present");
        }
        tunedEngine->stop();
    }
    vasdk::engine::EngineConfig badAudio;
    badAudio.mutable_audio()->mutable_input()->set_sample_rate(0);
    check(!VAEngine::create(badAudio, registry, options, &error) && contains(error, "audio.input.sample_rate"),
          "an impossible sample rate is reported with its field");

    // audio.tts_prebuffer_ms reaches the TTS factory; unset is -1, the voice's own default.
    {
        int seenPrebufferMs = -2;
        auto probing = makeRegistry(tts);
        probing.add<audio::GenericVoiceSynthesisProvider>("probe", [tts, &seenPrebufferMs](const engine::ProviderContext& context) {
            seenPrebufferMs = context.ttsPrebufferMs;
            return tts;
        });
        vasdk::engine::EngineConfig prebuffer;
        prebuffer.mutable_providers()->mutable_tts()->set_id("probe");
        options.transport = std::make_shared<FakeTransport>();
        check(VAEngine::create(prebuffer, probing, options, &error) && seenPrebufferMs == -1,
              "tts_prebuffer_ms unset: the voice's default");
        prebuffer.mutable_audio()->set_tts_prebuffer_ms(400);
        options.transport = std::make_shared<FakeTransport>();
        check(VAEngine::create(prebuffer, probing, options, &error) && seenPrebufferMs == 400,
              "tts_prebuffer_ms reaches the TTS");
        prebuffer.mutable_audio()->set_tts_prebuffer_ms(6000);
        check(!VAEngine::create(prebuffer, probing, options, &error) && contains(error, "audio.tts_prebuffer_ms"),
              "an out-of-range tts_prebuffer_ms is reported with its field");
    }

    // The config's webrtc: a browser brings the audio, through the registry's peer; no app transport needed.
    {
        auto peer = std::make_shared<FakePeer>();
        auto withPeer = makeRegistry(tts);
        withPeer.add<network::WebRtcInterface>("fake", [peer](const engine::ProviderContext&) { return peer; });
        vasdk::engine::EngineConfig webrtc;
        webrtc.mutable_providers()->mutable_webrtc()->set_id("fake");
        auto browserEngine = VAEngine::create(webrtc, withPeer, VAEngine::Options{}, &error);
        check(browserEngine != nullptr, "an engine over the config's WebRTC peer, without an app transport");
        if (browserEngine) {
            check(browserEngine->start(&error) && peer->opened, "the peer is opened at the engine's rates");
            browserEngine->stop();
        }
        check(!VAEngine::create(webrtc, registry, VAEngine::Options{}, &error) && contains(error, "webrtc:"),
              "a WebRTC peer that isn't built in is reported");
    }

    if (failures == 0) std::printf("engine_assembly_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
