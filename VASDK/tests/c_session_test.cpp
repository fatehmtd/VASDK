// The C API's spoken sessions: created from a config file and an agent, with the app's own
// audio. Events come as JSON, the assistant's speech through the audio callback, the user's
// words by inject_text (or pushed audio), and a snapshot resumes the conversation. Only C API
// calls, but for the fake providers registered through vasdk_cpp.h.

#include "engine_test_fakes.h"

#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>

#include <nlohmann/json.hpp>
#include <vasdk/engine/events.pb.h>
#include <vasdk/engine/session.pb.h>
#include <vasdk/flow/snapshot.pb.h>

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

// The same, each event the vasdk.engine.Event itself, serialized.
void onEventMessage(const void* data, size_t length, void* user) {
    auto& heard = *static_cast<Heard*>(user);
    vasdk::engine::Event event;
    if (!event.ParseFromArray(data, static_cast<int>(length)) || !event.has_agent_reply() || !event.agent_reply().is_final()) return;
    std::lock_guard<std::mutex> lock(heard.mutex);
    heard.replies.push_back(event.agent_reply().text());
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
    // The same as the vasdk.flow.ConversationSnapshot itself.
    size_t snapshotLength = 0;
    void* snapshotData = vasdk_session_snapshot_message(session, VASDK_FORMAT_BINARY, &snapshotLength);
    vasdk::flow::ConversationSnapshot message;
    check(message.ParseFromArray(snapshotData, static_cast<int>(snapshotLength)) && message.page() == "booking" &&
              message.form().slots().fields().count("day") == 1,
          "a snapshot as a message, with the form's filled slot");
    vasdk_session_free(session);

    Heard again;
    vasdk_session* later = vasdk_session_create(configPath.c_str(), providers, agent, nullptr, &error);
    vasdk_session_on_event(later, onEvent, &again);
    vasdk_session_resume(later, snapshot);
    vasdk_free(snapshot);
    check(vasdk_session_start(later, &error) == 1, "the later session starts");
    check(again.waitFor("Welcome back. Your name?"), "it resumes: only what's missing is asked");
    vasdk_session_free(later);

    Heard fromMessage;
    vasdk_session* resumed = vasdk_session_create(configPath.c_str(), providers, agent, nullptr, &error);
    vasdk_session_on_event(resumed, onEvent, &fromMessage);
    check(vasdk_session_resume_message(resumed, snapshotData, snapshotLength, VASDK_FORMAT_BINARY, &error) == 1,
          "a session resumes from the message");
    vasdk_free(snapshotData);
    check(vasdk_session_resume_message(resumed, "\x01\x02", 2, VASDK_FORMAT_BINARY, &error) == 0 && error,
          "bytes that aren't a snapshot fail");
    vasdk_free(error);
    error = nullptr;
    check(vasdk_session_start(resumed, &error) == 1, "it starts");
    check(fromMessage.waitFor("Welcome back. Your name?"), "and picks up the form where the message left it");
    vasdk_session_free(resumed);

    // The same through protos, serialized: SessionOptions in, Events out, a Command, a Struct context, a ToolDeclaration.
    vasdk::engine::SessionOptions options;
    options.set_id("c-proto");
    const std::string optionsData = options.SerializeAsString();
    vasdk_session* typed = vasdk_session_create_message(configPath.c_str(), providers, agent, optionsData.data(), optionsData.size(),
                                                        VASDK_FORMAT_BINARY, &error);
    check(typed != nullptr, std::string("a session from SessionOptions: ") + (error ? error : ""));
    if (typed) {
        Heard typedHeard;
        vasdk_session_on_event_message(typed, onEventMessage, VASDK_FORMAT_BINARY, &typedHeard);
        vasdk::engine::ToolDeclaration tool;
        tool.set_name("lookup_hours");
        tool.add_parameters()->set_name("day");
        const std::string toolData = tool.SerializeAsString();
        check(vasdk_session_add_tool_message(typed, toolData.data(), toolData.size(), VASDK_FORMAT_BINARY,
                                             [](const char*, vasdk_result* result, void*) { vasdk_result_set(result, "{}"); }, nullptr,
                                             &error) == 1,
              "a tool from a ToolDeclaration");
        check(vasdk_session_start(typed, &error) == 1, "it starts");
        check(typedHeard.waitFor("How can I help?"), "the greeting, as an Event");
        vasdk::engine::Command command;
        command.set_inject_text("when are you open");
        const std::string commandData = command.SerializeAsString();
        check(vasdk_session_send(typed, commandData.data(), commandData.size(), VASDK_FORMAT_BINARY, &error) == 1, "a Command is taken");
        check(typedHeard.waitFor("We're open 8 to 6. How can I help?"), "and answered");
        check(vasdk_session_send(typed, "", 0, VASDK_FORMAT_BINARY, &error) == 0 && error, "an empty Command fails");
        vasdk_free(error);
        error = nullptr;
        google::protobuf::Struct context;
        (*context.mutable_fields())["kitchen"].set_string_value("open");
        const std::string contextData = context.SerializeAsString();
        check(vasdk_session_set_context_message(typed, contextData.data(), contextData.size(), VASDK_FORMAT_BINARY, &error) == 1,
              "the context from a Struct");
        vasdk_session_free(typed);
    }

    // An engine config the app built, no file: as a serialized message and as text.
    vasdk::engine::EngineConfig built;
    built.mutable_providers()->mutable_stt()->set_id("stt");
    built.mutable_providers()->mutable_tts()->set_id("tts");
    const std::string builtData = built.SerializeAsString();
    vasdk_session* fromConfig = vasdk_session_create_config(builtData.data(), builtData.size(), VASDK_FORMAT_BINARY, providers, agent,
                                                            nullptr, 0, VASDK_FORMAT_BINARY, &error);
    check(fromConfig != nullptr, std::string("a session from an EngineConfig message: ") + (error ? error : ""));
    if (fromConfig) {
        Heard configHeard;
        vasdk_session_on_event_message(fromConfig, onEventMessage, VASDK_FORMAT_BINARY, &configHeard);
        check(vasdk_session_start(fromConfig, &error) == 1 && configHeard.waitFor("How can I help?"), "it starts and greets");
        vasdk_session_free(fromConfig);
    }
    const std::string builtText = R"(providers { stt { id: "stt" } tts { id: "tts" } })";
    fromConfig = vasdk_session_create_config(builtText.data(), builtText.size(), VASDK_FORMAT_TEXT, providers, agent, nullptr, 0,
                                             VASDK_FORMAT_BINARY, &error);
    check(fromConfig != nullptr, std::string("a session from an EngineConfig in text: ") + (error ? error : ""));
    vasdk_session_free(fromConfig);
    const std::string broken = R"(providers { stt { no_such_field: 1 } })";
    check(vasdk_session_create_config(broken.data(), broken.size(), VASDK_FORMAT_TEXT, providers, agent, nullptr, 0,
                                      VASDK_FORMAT_BINARY, &error) == nullptr && error && std::string(error).find("EngineConfig") != std::string::npos,
          "a config that doesn't parse is reported as the EngineConfig");
    vasdk_free(error);
    error = nullptr;
    vasdk::engine::EngineConfig floatAudio = built;
    floatAudio.mutable_audio()->mutable_output()->set_encoding(vasdk::engine::AudioFormat::FLOAT_32);
    const std::string floatData = floatAudio.SerializeAsString();
    check(vasdk_session_create_config(floatData.data(), floatData.size(), VASDK_FORMAT_BINARY, providers, agent, nullptr, 0,
                                      VASDK_FORMAT_BINARY, &error) == nullptr && error && std::string(error).find("config: audio") != std::string::npos,
          "a built config's errors name it \"config\"");
    vasdk_free(error);
    error = nullptr;
    vasdk_runtime* runtime = vasdk_runtime_create_config(builtData.data(), builtData.size(), VASDK_FORMAT_BINARY, providers, nullptr, 2, &error);
    check(runtime != nullptr, std::string("a runtime from an EngineConfig message: ") + (error ? error : ""));
    if (runtime) vasdk_runtime_free(runtime);

    vasdk_agent_free(agent);
    vasdk_providers_free(providers);
    std::filesystem::remove_all(dir);
    if (failures == 0) std::printf("c_session_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
