// VAEngine assembly: providers come from the registry by id, the standard chain is built in
// order with app processors at their slots, the LLM stages appear only with an `llm` endpoint,
// a missing provider fails create() with its kind, and say() text reaches the TTS provider.

#include <VASDK/VAEngine.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/Transport.h>
#include <VASDK/VADProvider.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

class FakeTransport : public transport::AudioTransport {
public:
    std::string getName() const override { return "FakeTransport"; }
    std::string getDescription() const override { return "test"; }
    bool initialize() override { return true; }
    bool start() override { setState(transport::TransportState::CONNECTED); return true; }
    void stop() override { setState(transport::TransportState::DISCONNECTED); }
    void release() override {}
    bool sendFrame(std::shared_ptr<transport::Frame>) override { return true; }
    void setInputAudioConfig(const audio::AudioConfig&) override {}
    void setOutputAudioConfig(const audio::AudioConfig&) override {}
    void setFrameSize(size_t samples) override { frameSize = samples; }
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> buffer) override { ringBuffer = buffer; }
    size_t frameSize = 0;
    std::shared_ptr<AudioRingBuffer> ringBuffer;
};

class FakeVad : public audio::VADProvider {
public:
    bool initialize(const audio::AudioConfig&, int) override { return true; }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char*, size_t, size_t) const override { return {}; }
};

class FakeStt : public audio::GenericTranscriptionProvider {
public:
    bool initializeTranscriptionProvider() override { return true; }
    void releaseTranscriptionProvider() override {}
    void setAudioConfiguration(const audio::AudioConfig& config) override { _config = config; }
    audio::AudioConfig getAudioConfiguration() const override { return _config; }
    size_t writeAudioData(const uint8_t*, size_t size) override { return size; }
    void setTranscriptionResultListener(audio::TranscriptionResultListener* listener) override { _listener = listener; }
    audio::TranscriptionResultListener* getTranscriptionResultListener() const override { return _listener; }
    bool startTranscriptionProvider() override { _running = true; return true; }
    void stopTranscriptionProvider() override { _running = false; }
    bool isTranscriptionProviderRunning() const override { return _running; }

private:
    audio::AudioConfig _config{};
    audio::TranscriptionResultListener* _listener = nullptr;
    bool _running = false;
};

// Records what it is asked to say; a worker plays 100 ms of audio per call, like a local TTS.
class FakeTts : public audio::GenericVoiceSynthesisProvider {
public:
    ~FakeTts() override { stopVoiceSynthesisProvider(); }
    bool synthesizeSpeech(const std::string& text) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _received.push_back(text);
        _pending.push_back(text);
        _wake.notify_all();
        return true;
    }
    void cancelSynthesis() override { std::lock_guard<std::mutex> lock(_mutex); _pending.clear(); }
    bool startVoiceSynthesisProvider() override {
        _running = true;
        _worker = std::thread([this] { loop(); });
        return true;
    }
    void stopVoiceSynthesisProvider() override {
        { std::lock_guard<std::mutex> lock(_mutex); if (!_running) return; _running = false; }
        _wake.notify_all();
        if (_worker.joinable()) _worker.join();
    }
    bool isVoiceSynthesisProviderRunning() const override { return _running; }
    void setAudioConfiguration(const audio::AudioConfig& config) override { _config = config; }
    audio::AudioConfig getAudioConfiguration() const override { return _config; }
    void setVoiceSynthesisResultListener(audio::VoiceSynthesisResultListener* listener) override { _listener = listener; }
    audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override { return _listener; }

    std::vector<std::string> said() { std::lock_guard<std::mutex> lock(_mutex); return _received; }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(_mutex);
        while (true) {
            _wake.wait(lock, [this] { return !_running || !_pending.empty(); });
            if (!_running) return;
            _pending.clear();
            lock.unlock();
            std::vector<char> pcm(3200, 1);
            _listener->onSynthesisStarted();
            _listener->onAudioDataReceived(pcm.data(), pcm.size());
            _listener->onSynthesisCompleted();
            lock.lock();
        }
    }

    std::vector<std::string> _received;
    std::vector<std::string> _pending;
    audio::AudioConfig _config{};
    audio::VoiceSynthesisResultListener* _listener = nullptr;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::atomic<bool> _running{false};
    std::thread _worker;
};

class Named : public pipeline::FrameProcessor {
public:
    explicit Named(std::string name) : _name(std::move(name)) {}
    std::string getName() const override { return _name; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override {
        if (direction == pipeline::FlowDirection::DOWNSTREAM) pushDownstream(frame); else pushUpstream(frame);
    }

private:
    std::string _name;
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool before(const std::string& text, const std::string& first, const std::string& second) {
    return contains(text, first) && contains(text, second) && text.find(first) < text.find(second);
}

} // namespace

int main() {
    auto tts = std::make_shared<FakeTts>();
    engine::ProviderRegistry registry;
    registry.add<audio::VADProvider>("fake", [](const engine::ProviderContext&) { return std::make_shared<FakeVad>(); });
    registry.add<audio::GenericTranscriptionProvider>("fake", [](const engine::ProviderContext&) {
        return std::make_shared<FakeStt>(); });
    registry.add<audio::GenericVoiceSynthesisProvider>("fake", [tts](const engine::ProviderContext&) { return tts; });

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

    if (failures == 0) std::printf("engine_assembly_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
