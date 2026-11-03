// Runtime + Session: two sessions at once are isolated (own providers, ring buffer, LLM history,
// events stamped with their id); an ended session's providers are reused, not reloaded, and work
// again; maxSessions is enforced; SessionManager finds, rejects duplicate ids and ends sessions.

#include "engine_test_fakes.h"

#include <VASDK/SessionManager.h>
#include <VASDK/engine/Runtime.h>
#include "vasdk/engine/events.pb.h"

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

bool said(FakeTts& tts, const std::string& text) {
    for (int i = 0; i < 100; ++i) {
        for (const auto& t : tts.said()) if (t.find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

bool saidSoFar(FakeTts& tts, const std::string& text) {
    for (const auto& t : tts.said()) if (t.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    std::mutex mutex;
    std::vector<std::shared_ptr<FakeTts>> ttsLoaded;   // one per provider set the runtime created
    engine::ProviderRegistry registry;
    registry.add<audio::VADProvider>("fake", [](const engine::ProviderContext&) { return std::make_shared<FakeVad>(); });
    registry.add<audio::GenericTranscriptionProvider>("fake", [](const engine::ProviderContext&) {
        return std::make_shared<FakeStt>(); });
    registry.add<audio::GenericVoiceSynthesisProvider>("fake", [&](const engine::ProviderContext&) {
        auto tts = std::make_shared<FakeTts>();
        std::lock_guard<std::mutex> lock(mutex);
        ttsLoaded.push_back(tts);
        return tts;
    });

    vasdk::engine::EngineConfig config;
    config.mutable_llm()->set_endpoint("http://127.0.0.1:9/v1/chat/completions");
    config.mutable_llm()->set_api_key("test");
    engine::Runtime::Options limits;
    limits.maxSessions = 2;
    std::string error;
    auto runtime = engine::Runtime::create(config, registry, limits, &error);
    if (!runtime) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
    check(ttsLoaded.size() == 1 && runtime->idleProviderSets() == 1, "runtime loads one provider set up front");

    std::mutex eventsMutex;
    std::vector<vasdk::engine::Event> eventsA;
    {
        SessionManager calls(runtime);
        engine::Session::Options a;
        a.id = "call-a";
        a.transport = std::make_shared<FakeTransport>();
        engine::Session::Options b = a;
        b.id = "call-b";
        b.transport = std::make_shared<FakeTransport>();

        auto sessionA = calls.createSession(a, &error);
        auto sessionB = calls.createSession(b, &error);
        check(sessionA && sessionB, "two sessions created");
        if (!sessionA || !sessionB) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        check(ttsLoaded.size() == 2 && runtime->activeSessions() == 2, "second concurrent session loads its own set");
        check(sessionA->inputRingBuffer() != sessionB->inputRingBuffer(), "own ring buffers");
        check(sessionA->llmTransport() && sessionA->llmTransport() != sessionB->llmTransport(), "own LLM history");

        engine::Session::Options c = a;
        c.id = "call-c";
        c.transport = std::make_shared<FakeTransport>();
        check(!calls.createSession(c, &error) && error.find("in use") != std::string::npos, "maxSessions enforced");
        engine::Session::Options duplicate = a;
        duplicate.transport = std::make_shared<FakeTransport>();
        check(!calls.createSession(duplicate, &error) && error.find("already running") != std::string::npos,
              "duplicate id rejected");
        engine::Session::Options unnamed = a;
        unnamed.id.clear();
        check(!calls.createSession(unnamed, &error) && error.find("id is required") != std::string::npos,
              "id required");
        check(calls.session("call-b") == sessionB && calls.activeSessionCount() == 2, "sessions found by id");

        sessionA->addListener([&](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            eventsA.push_back(event);
        });
        check(sessionA->start(&error) && sessionB->start(&error), "both sessions start");
        sessionA->say("Hello from A.");
        sessionB->say("Hello from B.");
        // Sets were loaded in order: A took the first, B the second.
        check(said(*ttsLoaded[0], "Hello from A.") && said(*ttsLoaded[1], "Hello from B."), "each session speaks");
        check(!saidSoFar(*ttsLoaded[0], "from B") && !saidSoFar(*ttsLoaded[1], "from A"), "speech stays in its session");
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            bool onlyA = !eventsA.empty();
            for (const auto& event : eventsA)
                onlyA = onlyA && event.session_id() == "call-a" &&
                        (!event.has_agent_reply() || event.agent_reply().text().find("from B") == std::string::npos);
            check(onlyA, "events carry their session id and only that session's activity");
        }

        sessionA.reset();
        calls.destroySession("call-a");
        check(runtime->activeSessions() == 1 && runtime->idleProviderSets() == 1, "ended session's providers go back");

        auto sessionC = calls.createSession(c, &error);
        check(sessionC && ttsLoaded.size() == 2, "a new session reuses them: no reload");
        check(sessionC && sessionC->start(&error), "reused providers start again");
        if (sessionC) {
            sessionC->say("Hello from C.");
            check(said(*ttsLoaded[0], "Hello from C."), "reused providers speak");
        }
    }   // the manager stops what is still running
    check(runtime->activeSessions() == 0 && runtime->idleProviderSets() == 2, "all providers back after shutdown");

    if (failures == 0) std::printf("session_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
