// VAEngine events: listeners get say() replies and agent states from a running engine, the
// user's injected text as a transcript, and an LLM failure as an error; the dispatcher delivers
// in order, survives a throwing listener, and stops calling a removed one.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>
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

// Collects events from the engine's event thread.
struct Recorder {
    std::mutex mutex;
    std::vector<vasdk::engine::Event> events;

    engine::EventListener listener() {
        return [this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
    }
    bool waitFor(const std::function<bool(const vasdk::engine::Event&)>& match) {
        for (int i = 0; i < 150; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& event : events) if (match(event)) return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
};

} // namespace

int main() {
    // Dispatcher: in order, a throwing listener doesn't stop the others, removal is honoured.
    {
        std::vector<int> seen;
        {
            engine::EventDispatcher dispatcher;
            dispatcher.addListener([](const vasdk::engine::Event&) { throw std::runtime_error("listener bug"); });
            const int id = dispatcher.addListener([&seen](const vasdk::engine::Event& event) {
                seen.push_back(event.error().code());
            });
            for (int code = 1; code <= 3; ++code) {
                vasdk::engine::Event event;
                event.mutable_error()->set_code(code);
                dispatcher.post(event);
            }
            std::this_thread::sleep_for(100ms);
            dispatcher.removeListener(id);
            vasdk::engine::Event late;
            late.mutable_error()->set_code(4);
            dispatcher.post(late);
        }   // the destructor delivers what is queued
        check(seen == std::vector<int>({1, 2, 3}), "dispatcher delivers in order, skips a removed listener");
    }

    auto tts = std::make_shared<FakeTts>();
    const auto registry = makeRegistry(tts);
    VAEngine::Options options;
    std::string error;

    // say(): the reply and the agent's state changes reach a listener added before start().
    {
        Recorder recorder;   // outlives the engine, whose event thread calls it
        options.transport = std::make_shared<FakeTransport>();
        auto engine = VAEngine::create(vasdk::engine::EngineConfig{}, registry, options, &error);
        if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        engine->addListener(recorder.listener());
        check(engine->describe().find("SpeechInput → EventTap → TTSFrameProcessor") != std::string::npos,
              "event tap sits just before TTS");
        check(engine->start(&error), "engine starts");
        engine->say("Hello there.");
        check(recorder.waitFor([](const auto& event) {
            return event.has_agent_reply() && event.agent_reply().is_final() &&
                   event.agent_reply().text() == "Hello there." && event.timestamp_ms() > 0;
        }), "say() reported as a final agent reply");
        check(recorder.waitFor([](const auto& event) {
            return event.has_agent_state() && event.agent_state().state() == "LISTENING" &&
                   event.agent_state().transition() == "PIPELINE_START";
        }), "agent state change reported");
        engine->stop();
    }

    // injectUserText(): the user's turn is a transcript; an unreachable LLM is an error event.
    {
        vasdk::engine::EngineConfig online;
        online.mutable_llm()->set_endpoint("http://127.0.0.1:9/v1/chat/completions");
        online.mutable_llm()->set_model("test");
        online.mutable_llm()->set_api_key("test");
        Recorder recorder;
        options.transport = std::make_shared<FakeTransport>();
        auto engine = VAEngine::create(online, registry, options, &error);
        if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        engine->addListener(recorder.listener());
        check(engine->start(&error), "LLM engine starts");
        engine->injectUserText("what time is it");
        check(recorder.waitFor([](const auto& event) {
            return event.has_transcript() && event.transcript().is_final() &&
                   event.transcript().text() == "what time is it";
        }), "injected text reported as a final transcript");
        check(recorder.waitFor([](const auto& event) {
            return event.has_error() && event.error().source() == "OpenAITransport";
        }), "LLM failure reported as an error");
        engine->stop();
    }

    if (failures == 0) std::printf("engine_events_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
