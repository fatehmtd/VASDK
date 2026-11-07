// Providers and processors written in C (vasdk_providers_add_*, vasdk_session_add_*_processor):
// a whole spoken session with no models. Typed words go to the app's LLM, its reply through the
// app's text processor to the app's TTS and out of the audio callback; pushed audio goes through
// the app's VAD to the app's STT, whose transcript the app's turn model ends. The app's context,
// the new Commands and queries, and a runtime with the app's sentence encoder and entity model.
// Only C API calls.

#include <VASDK/c/vasdk.h>

#include <nlohmann/json.hpp>
#include <vasdk/engine/events.pb.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

bool waitFor(const std::function<bool()>& condition, int ms = 5000) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (condition()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return condition();
}

// What the app's providers and callbacks saw, shared by every instance.
struct App {
    std::mutex mutex;
    std::vector<std::string> events;       // every event, as JSON
    std::vector<std::string> llmRequests;
    std::vector<std::string> spoken;       // what the TTS was asked to say
    std::atomic<int> created{0}, destroyed{0};
    std::atomic<size_t> sttSamples{0}, micSamples{0}, playedSamples{0};
    std::atomic<int> turnAsked{0};
    bool sawEvent(const std::string& part) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& event : events) if (event.find(part) != std::string::npos) return true;
        return false;
    }
};

struct Instance {
    App* app;
    vasdk_sink* sink;
};

App* appOf(void* instance) { return static_cast<Instance*>(instance)->app; }

void* create(vasdk_sink* sink, int, void* user) {
    auto* app = static_cast<App*>(user);
    ++app->created;
    return new Instance{app, sink};
}
void destroy(void* instance) {
    ++appOf(instance)->destroyed;
    delete static_cast<Instance*>(instance);
}

// VAD: loud is speech.
float detect(void*, const short* pcm, size_t samples) {
    int loudest = 0;
    for (size_t i = 0; i < samples; ++i) loudest = std::max(loudest, std::abs(static_cast<int>(pcm[i])));
    return loudest > 1000 ? 0.9f : 0.0f;
}

// STT: whatever was said is "what time is it", final at the end of speech.
void sttWrite(void* instance, const short*, size_t samples) { appOf(instance)->sttSamples += samples; }
void sttEnd(void* instance) {
    auto* self = static_cast<Instance*>(instance);
    vasdk_sink_text(self->sink, "what", 0);
    vasdk_sink_text(self->sink, "what time is it", 1);
}

// TTS: 100 ms of a quiet tone per text, said at once.
int synthesize(void* instance, const char* text) {
    auto* self = static_cast<Instance*>(instance);
    {
        std::lock_guard<std::mutex> lock(self->app->mutex);
        self->app->spoken.push_back(text);
    }
    std::vector<short> pcm(1600, 100);
    vasdk_sink_audio(self->sink, pcm.data(), pcm.size());
    vasdk_sink_done(self->sink);
    return 1;
}

// End of turn: sure, once asked.
float turnEnd(void* instance, const short*, size_t) {
    ++appOf(instance)->turnAsked;
    return 0.9f;
}

// LLM: echoes the last user message.
void complete(void* instance, const char* request_json, vasdk_result* reply) {
    auto* app = appOf(instance);
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        app->llmRequests.push_back(request_json);
    }
    const auto request = nlohmann::json::parse(request_json);
    std::string last;
    for (const auto& message : request["messages"])
        if (message["role"] == "user") last = message["content"];
    vasdk_result_set(reply, ("You said: " + last + ".").c_str());
}

void encode(void*, const char* sentence, vasdk_result* vector) {
    const float values[3] = {static_cast<float>(std::strlen(sentence)), 0.0f, 1.0f};
    vasdk_result_set_floats(vector, values, 3);
}

void extract(void*, const char*, vasdk_result* spans) { vasdk_result_set(spans, "[]"); }

void onEvent(const char* json, void* user) {
    auto* app = static_cast<App*>(user);
    std::lock_guard<std::mutex> lock(app->mutex);
    app->events.push_back(json);
}
void onAudio(const short*, size_t samples, void* user) { static_cast<App*>(user)->playedSamples += samples; }
void onMic(short*, size_t samples, int, void* user) { static_cast<App*>(user)->micSamples += samples; }
void louder(short* pcm, size_t samples, int, void*) {
    for (size_t i = 0; i < samples; ++i) pcm[i] = static_cast<short>(pcm[i] * 2);
}
// The reply's words: "You said" becomes "I heard".
void rewrite(const char* text, vasdk_result* result, void*) {
    std::string words = text;
    if (const auto at = words.find("You said"); at != std::string::npos) {
        words.replace(at, 8, "I heard");
        vasdk_result_set(result, words.c_str());
    }
}
void context(vasdk_result* result, void*) { vasdk_result_set(result, R"({"city": "Paris"})"); }

std::string configFile(const std::string& providers) {
    const auto path = std::filesystem::temp_directory_path() / ("vasdk_c_providers_" + std::to_string(std::rand()) + ".txtpb");
    std::ofstream(path) << "providers { " << providers << " }\n";
    return path.string();
}

void pushAudio(vasdk_session* session, int milliseconds, short amplitude) {
    const int rate = vasdk_session_input_rate(session);
    std::vector<short> pcm(static_cast<size_t>(rate / 100));
    for (int chunk = 0; chunk < milliseconds / 10; ++chunk) {
        for (size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<short>(amplitude * std::sin(2.0 * 3.14159265 * 440.0 * static_cast<double>(i) / rate));
        vasdk_session_push_audio(session, pcm.data(), pcm.size());
        std::this_thread::sleep_for(10ms);
    }
}

std::string take(char* text) {
    std::string out = text ? text : "";
    vasdk_free(text);
    return out;
}

void testSession(App& app) {
    vasdk_providers* providers = vasdk_providers_create();
    check(vasdk_providers_add_vad(providers, "app", create, destroy, detect, &app), "add vad");
    check(vasdk_providers_add_stt(providers, "app", create, destroy, nullptr, sttWrite, sttEnd, nullptr, &app), "add stt");
    check(vasdk_providers_add_tts(providers, "app", create, destroy, synthesize, nullptr, &app), "add tts");
    check(vasdk_providers_add_turn(providers, "app", create, destroy, turnEnd, &app), "add turn");
    check(vasdk_providers_add_llm(providers, "app", create, destroy, complete, nullptr, &app), "add llm");
    check(!vasdk_providers_add_vad(providers, "app", create, destroy, nullptr, &app), "a provider without its callback is refused");

    const auto config = configFile(R"(vad { id: "app" } stt { id: "app" } tts { id: "app" } turn { id: "app" } llm { id: "app" })");
    char* error = nullptr;
    vasdk_session* session = vasdk_session_create(config.c_str(), providers, nullptr,
                                                  R"({"system_prompt": "Be brief. The user is in {context}."})", &error);
    check(session != nullptr, std::string("a session on the app's providers: ") + (error ? error : ""));
    vasdk_free(error);
    error = nullptr;
    if (!session) return;
    vasdk_session_on_event(session, onEvent, &app);
    vasdk_session_on_audio(session, onAudio, &app);
    vasdk_session_on_context(session, context, nullptr);
    check(vasdk_session_add_audio_processor(session, VASDK_SLOT_AFTER_INPUT, onMic, &app), "an AFTER_INPUT audio processor");
    check(vasdk_session_add_audio_processor(session, VASDK_SLOT_BEFORE_OUTPUT, louder, nullptr), "a BEFORE_OUTPUT audio processor");
    check(vasdk_session_add_text_processor(session, VASDK_SLOT_BEFORE_SYNTHESIS, rewrite, nullptr), "a BEFORE_SYNTHESIS text processor");
    check(!vasdk_session_add_text_processor(session, VASDK_SLOT_AFTER_INPUT, rewrite, nullptr), "a text processor at an audio slot is refused");
    check(vasdk_session_start(session, &error), "the session starts");
    check(!vasdk_session_add_text_processor(session, VASDK_SLOT_AFTER_TRANSCRIPTION, rewrite, nullptr), "no processors once started");

    // Typed words: the app's LLM, its reply rewritten, said by the app's TTS
    vasdk_session_inject_text(session, "hello");
    check(waitFor([&] {
              std::lock_guard<std::mutex> lock(app.mutex);
              for (const auto& said : app.spoken) if (said.find("I heard: hello.") != std::string::npos) return true;
              return false;
          }),
          "the app's LLM answered, the text processor rewrote it, the app's TTS said it");
    check(waitFor([&] { return app.playedSamples >= 1600; }), "the TTS's audio came out of the audio callback");
    {
        std::lock_guard<std::mutex> lock(app.mutex);
        check(!app.llmRequests.empty() && app.llmRequests.front().find("Be brief.") != std::string::npos &&
                  app.llmRequests.front().find("Paris") != std::string::npos,
              "the LLM got the system prompt, {context} filled from on_context");
    }

    // Spoken words: the app's VAD, STT and turn model
    pushAudio(session, 300, 0);
    pushAudio(session, 1200, 8000);
    pushAudio(session, 1500, 0);
    check(waitFor([&] { return app.sawEvent("what time is it"); }), "the app's STT's final transcript is an event");
    check(app.sttSamples > 0 && app.micSamples > 0, "the STT and the AFTER_INPUT processor got the microphone's audio");
    check(app.turnAsked > 0, "the app's turn model was asked");
    check(waitFor([&] {
              std::lock_guard<std::mutex> lock(app.mutex);
              for (const auto& request : app.llmRequests) if (request.find("what time is it") != std::string::npos) return true;
              return false;
          }),
          "the spoken turn reached the LLM");

    // Queries and commands
    check(take(vasdk_session_describe(session)).find("app") != std::string::npos, "describe names the app's providers");
    check(!vasdk_session_using_fallbacks(session), "no fallbacks in use");
    check(vasdk_session_next_scheduled_trigger_ms(session) == -1, "no agent, no schedule trigger");
    vasdk::engine::Command command;
    command.set_reset_conversation(true);
    const std::string bytes = command.SerializeAsString();
    check(vasdk_session_send(session, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &error), "reset_conversation through send");
    command.Clear();
    command.set_push_to_talk(true);
    const std::string ptt = command.SerializeAsString();
    check(vasdk_session_send(session, ptt.data(), ptt.size(), VASDK_FORMAT_BINARY, &error), "push_to_talk through send");

    vasdk_session_free(session);
    vasdk_providers_free(providers);
    std::filesystem::remove(config);
    check(app.created > 0 && app.created == app.destroyed, "every instance the session made was destroyed");
}

void testRuntime(App& app) {
    vasdk_providers* providers = vasdk_providers_create();
    vasdk_providers_add_vad(providers, "app", create, destroy, detect, &app);
    vasdk_providers_add_stt(providers, "app", create, destroy, nullptr, sttWrite, sttEnd, nullptr, &app);
    vasdk_providers_add_tts(providers, "app", create, destroy, synthesize, nullptr, &app);
    check(vasdk_providers_add_sentence_encoder(providers, "app", create, destroy, encode, &app), "add sentence encoder");
    check(vasdk_providers_add_entity_model(providers, "app", create, destroy, extract, &app), "add entity model");
    const auto config =
        configFile(R"(vad { id: "app" } stt { id: "app" } tts { id: "app" } sentence_encoder { id: "app" } entity_model { id: "app" })");
    const int before = app.created;
    char* error = nullptr;
    vasdk_runtime* runtime = vasdk_runtime_create(config.c_str(), providers, nullptr, 2, &error);
    check(runtime != nullptr, std::string("a runtime: ") + (error ? error : ""));
    vasdk_free(error);
    error = nullptr;
    if (!runtime) return;
    check(app.created - before == 5, "a runtime makes one provider set, the encoder and the entity model");
    vasdk_session* first = vasdk_runtime_session_create(runtime, nullptr, nullptr, 0, VASDK_FORMAT_BINARY, &error);
    vasdk_session* second = vasdk_runtime_session_create(runtime, nullptr, nullptr, 0, VASDK_FORMAT_BINARY, &error);
    check(first && second && vasdk_runtime_active_sessions(runtime) == 2, "two sessions from one runtime");
    vasdk_session* third = vasdk_runtime_session_create(runtime, nullptr, nullptr, 0, VASDK_FORMAT_BINARY, &error);
    check(!third && error, "max_sessions is enforced");
    vasdk_free(error);
    vasdk_session_free(first);
    check(vasdk_runtime_active_sessions(runtime) == 1 && vasdk_runtime_idle_provider_sets(runtime) == 1,
          "a freed session's providers wait for the next");
    vasdk_session_free(second);
    vasdk_runtime_free(runtime);
    vasdk_providers_free(providers);
    std::filesystem::remove(config);
}

} // namespace

int main() {
    vasdk_set_log_level("warn");
    App app;
    testSession(app);
    App shared;
    testRuntime(shared);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("C providers ok");
    return 0;
}
