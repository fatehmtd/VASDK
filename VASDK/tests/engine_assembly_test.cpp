// VAEngine assembly: providers come from the registry by id, the standard chain is built in
// order with app processors at their slots, the LLM stages appear only with an `llm` endpoint,
// a missing provider fails create() with its kind, and say() text reaches the TTS provider.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>

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

    if (failures == 0) std::printf("engine_assembly_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
