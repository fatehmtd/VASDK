// Activation modes from the engine config: ALWAYS_ON listens from the start; PUSH_TO_TALK waits
// for Session::pushToTalk; WAKE_WORD needs a wake-word provider (an error names it when none is
// built in) and puts the WakeWordProcessor before the VAD, asleep until the phrase.

#include "engine_test_fakes.h"

#include <VASDK/WakeWordProvider.h>
#include <VASDK/engine/Runtime.h>
#include "vasdk/engine/events.pb.h"

#include <chrono>
#include <cstdio>
#include <functional>

using namespace va;
using namespace fakes;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

class FakeWakeWord : public audio::WakeWordProvider {
public:
    bool initialize(const audio::AudioConfig&) override { return true; }
    std::optional<Detection> processAudio(const char*, size_t) override { return std::nullopt; }
};

struct Recorder {
    std::mutex mutex;
    std::vector<vasdk::engine::AgentStateChange> states;
    engine::EventListener listener() {
        return [this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(mutex);
            if (event.has_agent_state()) states.push_back(event.agent_state());
        };
    }
    bool saw(const std::string& state, const std::string& transition) {
        for (int i = 0; i < 50; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& s : states) if (s.state() == state && s.transition() == transition) return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
};

std::unique_ptr<engine::Session> start(const engine::ProviderRegistry& registry, vasdk::engine::Activation::Mode mode,
                                       Recorder& recorder, std::shared_ptr<engine::Runtime>& runtime, std::string& error) {
    vasdk::engine::EngineConfig config;
    config.mutable_activation()->set_mode(mode);
    runtime = engine::Runtime::create(config, registry, {}, &error);
    if (!runtime) return nullptr;
    engine::Session::Options options;
    options.transport = std::make_shared<FakeTransport>();
    auto session = runtime->createSession(options, &error);
    if (!session) return nullptr;
    session->addListener(recorder.listener());
    if (!session->start(&error)) return nullptr;
    return session;
}

} // namespace

int main() {
    auto registry = makeRegistry(std::make_shared<FakeTts>());
    std::string error;
    std::shared_ptr<engine::Runtime> runtime;
    {
        Recorder recorder;
        auto session = start(registry, vasdk::engine::Activation::ALWAYS_ON, recorder, runtime, error);
        check(session && recorder.saw("LISTENING", "PIPELINE_START"), "ALWAYS_ON listens from the start");
    }
    {
        Recorder recorder;
        auto session = start(registry, vasdk::engine::Activation::PUSH_TO_TALK, recorder, runtime, error);
        check(session != nullptr, "PUSH_TO_TALK session starts");
        if (session) {
            check(!recorder.saw("LISTENING", "PIPELINE_START"), "PUSH_TO_TALK starts asleep");
            session->pushToTalk(true);
            check(recorder.saw("LISTENING", "PUSH_TO_TALK"), "button held: listening");
            session->pushToTalk(false);
            check(recorder.saw("IDLE", "PUSH_TO_TALK"), "button released: asleep again");
        }
    }
    {
        Recorder recorder;
        check(!start(registry, vasdk::engine::Activation::WAKE_WORD, recorder, runtime, error) &&
              error.find("Wake word: ") != std::string::npos, "WAKE_WORD without a provider names the problem");
    }
    {
        registry.add<audio::WakeWordProvider>("fake", [](const engine::ProviderContext&) {
            return std::make_shared<FakeWakeWord>(); });
        Recorder recorder;
        auto session = start(registry, vasdk::engine::Activation::HYBRID, recorder, runtime, error);
        check(session != nullptr, "HYBRID session starts with a wake-word provider");
        if (session) {
            const auto chain = session->describe();
            check(chain.find("WakeWordProcessor (FakeWakeWord)") != std::string::npos &&
                  chain.find("WakeWordProcessor") < chain.find("VADFrameProcessor"), "wake word listens before the VAD");
            check(!recorder.saw("LISTENING", "PIPELINE_START"), "HYBRID starts asleep");
            session->pushToTalk(true);
            check(recorder.saw("LISTENING", "PUSH_TO_TALK"), "HYBRID also wakes on the button");
        }
    }

    if (failures == 0) std::printf("activation_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
