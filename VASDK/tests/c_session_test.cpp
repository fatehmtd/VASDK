// The C API's spoken sessions: created from a config file and an agent, with the app's own
// audio. Events come as JSON, the assistant's speech through the audio callback, the user's
// words by inject_text (or pushed audio), and a snapshot resumes the conversation. Only C API
// calls, but for the fake providers registered through vasdk_cpp.h.

#include "engine_test_fakes.h"

#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

struct Heard {
    std::mutex mutex;
    std::vector<std::string> replies;   // agentReply texts, final pieces
    std::atomic<size_t> samples{0};
    bool waitFor(const std::string& text) {
        for (int i = 0; i < 150; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& reply : replies) if (reply == text) return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
};

void onEvent(const char* json, void* user) {
    auto& heard = *static_cast<Heard*>(user);
    const auto event = nlohmann::json::parse(json, nullptr, false);
    if (!event.contains("agentReply") || !event["agentReply"].value("isFinal", false)) return;
    std::lock_guard<std::mutex> lock(heard.mutex);
    heard.replies.push_back(event["agentReply"].value("text", std::string()));
}

void onAudio(const short*, size_t samples, void* user) { static_cast<Heard*>(user)->samples += samples; }

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "book" examples: ["book"] }
  intents { name: "hours" examples: ["hours", "when are you open"] }
  goals { name: "booking" slots { name: "day" prompt: "Which day?" } slots { name: "name" prompt: "Your name?" } }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome" on_enter { ask { say { text: "How can I help?" } } }
            routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
            routes { intent: "book" target: "booking" } }
    pages { name: "booking" form { goal: "booking" }
            handlers { event: "resumed" steps { say { text: "Welcome back." } } } } }
)";

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "vasdk_c_session_test";
    std::filesystem::create_directories(dir);
    const auto configPath = (dir / "engine.txtpb").string();
    std::ofstream(configPath) << R"(providers { stt { id: "stt" } tts { id: "tts" } })";

    vasdk_providers* providers = vasdk_providers_create();
    auto& registry = vasdk_providers_registry(providers);
    registry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
    registry.add<audio::GenericTranscriptionProvider>("stt", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeStt>(); });
    auto voice = std::make_shared<fakes::FakeTts>();
    registry.add<audio::GenericVoiceSynthesisProvider>("tts", [&voice](const engine::ProviderContext&) { return voice; });

    char* error = nullptr;
    vasdk_agent* agent = vasdk_agent_parse(kAgent, std::strlen(kAgent), VASDK_FORMAT_TEXT, nullptr, &error);
    check(agent != nullptr, std::string("the agent parses: ") + (error ? error : ""));
    if (!agent) return 1;

    check(vasdk_session_create("no/such/config.txtpb", providers, agent, nullptr, &error) == nullptr && error,
          "a missing config: no session, and why");
    vasdk_free(error);
    error = nullptr;

    // A session: its greeting and replies as events, its speech through the audio callback.
    Heard heard;
    vasdk_session* session = vasdk_session_create(configPath.c_str(), providers, agent, R"({"id": "c-call"})", &error);
    check(session != nullptr, std::string("a session: ") + (error ? error : ""));
    if (!session) return 1;
    check(vasdk_session_input_rate(session) == 16000 && vasdk_session_output_rate(session) == 16000, "16 kHz in and out by default");
    vasdk_session_on_event(session, onEvent, &heard);
    vasdk_session_on_audio(session, onAudio, &heard);
    check(vasdk_session_start(session, &error) == 1, "starts");
    check(heard.waitFor("How can I help?"), "the greeting, as an agentReply event");
    for (int i = 0; i < 100 && heard.samples == 0; ++i) std::this_thread::sleep_for(20ms);
    check(heard.samples > 0, "its speech through the audio callback");
    vasdk_session_inject_text(session, "when are you open");
    check(heard.waitFor("We're open 8 to 6. How can I help?"), "a turn by text, answered");
    std::vector<short> pcm(2500, 0);   // not a whole number of frames: the rest waits for more
    vasdk_session_push_audio(session, pcm.data(), pcm.size());
    vasdk_session_push_audio(session, pcm.data(), pcm.size());
    check(vasdk_session_set_param(session, "nothing", "\"x\"", &error) == 1, "a param is set (an undeclared one is logged)");
    check(vasdk_session_set_param(session, "x", "not json", &error) == 0 && error, "a value that isn't JSON fails");
    vasdk_free(error);
    error = nullptr;

    // Halfway through a form: the snapshot resumes it in a new session.
    vasdk_session_inject_text(session, "book");
    check(heard.waitFor("Which day?"), "into the form");
    vasdk_session_inject_text(session, "tuesday");
    check(heard.waitFor("Your name?"), "one slot filled");
    char* snapshot = vasdk_session_snapshot(session);
    const auto saved = nlohmann::json::parse(snapshot, nullptr, false);
    check(saved.is_object() && saved.value("page", "") == "booking", std::string("a snapshot of the form: ") + snapshot);
    vasdk_session_free(session);

    Heard again;
    vasdk_session* later = vasdk_session_create(configPath.c_str(), providers, agent, nullptr, &error);
    vasdk_session_on_event(later, onEvent, &again);
    vasdk_session_resume(later, snapshot);
    vasdk_free(snapshot);
    check(vasdk_session_start(later, &error) == 1, "the later session starts");
    check(again.waitFor("Welcome back. Your name?"), "it resumes: only what's missing is asked");
    vasdk_session_free(later);

    vasdk_agent_free(agent);
    vasdk_providers_free(providers);
    std::filesystem::remove_all(dir);
    if (failures == 0) std::printf("c_session_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
