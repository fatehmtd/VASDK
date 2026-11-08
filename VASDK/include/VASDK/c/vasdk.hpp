// VASDK's C API in C++: header-only, over vasdk.h and nothing else, so it works with the prebuilt C
// library (vasdk_c, VASDK::c) from any compiler. Handles free themselves, text comes back as
// std::string, failures throw vasdk::Error, and callbacks are std::function. Whatever a C object
// must outlive (an agent its registry's callbacks, a session its providers and agent) is kept alive
// by the C++ object that uses it, so the order objects go out of scope doesn't matter.
//
//   #include <VASDK/c/vasdk.hpp>
//
//   vasdk::Registry registry;
//   registry.addTool("create_booking", "Books a slot", [](std::string_view args) { return R"({"id": 42})"; });
//   vasdk::Agent agent = vasdk::Agent::load("agents/garage/agent.txtpb", registry);
//   vasdk::Conversation conversation(agent);
//   std::string reply = conversation.start();            // {"text": "Hi, ...", ...}
//   reply = conversation.say("book an oil change");
//
//   vasdk::Providers providers = vasdk::Providers::builtIn();     // every provider in the library
//   vasdk::Models models;                                         // VASDK_MODELS_DIR, else the user cache
//   for (const auto& id : models.forConfig("engine.txtpb")) models.pull(id);
//   vasdk::Session session("engine.txtpb", providers, agent);
//   session.onEvent([](std::string_view event) { std::cout << event << '\n'; });
//   session.start();
//
// The full C++ API (VASDK/*.h, the C++ library built from source) is a different thing: it is
// richer but has no stable binary interface. This header is the C API's: what a release keeps
// (docs/reference/c-api.md). Anything it doesn't wrap is a call away: get() gives the C handle.
// C++17.

#ifndef VASDK_C_VASDK_HPP
#define VASDK_C_VASDK_HPP

#include "vasdk.h"
#include "vasdk_providers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vasdk {

/// What a failed call said
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

/// Text VASDK returned: copied, then freed
inline std::string take(char* text) {
    if (!text) return {};
    std::string out(text);
    vasdk_free(text);
    return out;
}

/// Throws the message a failed call set (and frees it)
[[noreturn]] inline void raise(char* error, const char* fallback) {
    const std::string message = error ? std::string(error) : std::string(fallback);
    if (error) vasdk_free(error);
    throw Error(message);
}

/// A JSON array of strings, as vasdk_models_for_config gives: ["a", "b"]. Ids have no escapes.
inline std::vector<std::string> stringArray(const std::string& json) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = json.find('"', at)) != std::string::npos) {
        const std::size_t end = json.find('"', at + 1);
        if (end == std::string::npos) break;
        out.push_back(json.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return out;
}

template <class Handle>
using Shared = std::shared_ptr<Handle>;

} // namespace detail

inline std::string version() { return vasdk_version(); }
/// The library's C API version: lower than VASDK_C_API_VERSION means this header is newer than it
inline int apiVersion() { return vasdk_c_api_version(); }
/// "trace", "debug", "info", "warn", "error" or "off": VASDK's and the providers' logs
inline void setLogLevel(const std::string& level) {
    if (!vasdk_set_log_level(level.c_str())) throw Error("unknown log level: " + level);
    vasdk_providers_set_log_level(level.c_str());
}

/// The turn a step runs in
class Turn {
public:
    explicit Turn(vasdk_turn* turn) : _turn(turn) {}
    std::string text() const { return vasdk_turn_text(_turn); }
    std::string replyText() const { return vasdk_turn_reply_text(_turn); }
    void say(const std::string& text) { vasdk_turn_say(_turn, text.c_str()); }
    void goTo(const std::string& target) { vasdk_turn_goto(_turn, target.c_str()); }
    void setText(const std::string& text) { vasdk_turn_set_text(_turn, text.c_str()); }
    std::string render(const std::string& text) const { return detail::take(vasdk_turn_render(_turn, text.c_str())); }
    std::string lookup(const std::string& reference) const { return detail::take(vasdk_turn_lookup(_turn, reference.c_str())); }
    std::string flow() const { return detail::take(vasdk_turn_flow(_turn)); }
    std::string page() const { return detail::take(vasdk_turn_page(_turn)); }
    vasdk_turn* get() const { return _turn; }

private:
    vasdk_turn* _turn;
};

/// Tools and steps written in C++, for the agents loaded with it
class Registry {
public:
    /// Gets the call's arguments as a JSON object and returns its result as JSON; a thrown
    /// exception fails the call (tool-error) with its message
    using Tool = std::function<std::string(std::string_view argumentsJson)>;
    /// Gets its params as JSON
    using Step = std::function<void(std::string_view paramsJson, Turn& turn)>;

    Registry() : _state(std::make_shared<State>()) {}

    void addTool(const std::string& name, const std::string& description, Tool tool) {
        _state->tools.push_back(std::make_unique<Tool>(std::move(tool)));
        if (!vasdk_registry_add_tool(_state->registry.get(), name.c_str(), description.c_str(), &callTool, _state->tools.back().get()))
            throw Error("tool not added: " + name);
    }
    void addStep(const std::string& paramsType, Step step) {
        _state->steps.push_back(std::make_unique<Step>(std::move(step)));
        if (!vasdk_registry_add_step(_state->registry.get(), paramsType.c_str(), &callStep, _state->steps.back().get()))
            throw Error("step not added: " + paramsType);
    }
    vasdk_registry* get() const { return _state->registry.get(); }

private:
    friend class Agent;
    friend class Session;
    struct State {
        std::unique_ptr<vasdk_registry, void (*)(vasdk_registry*)> registry{vasdk_registry_create(), &vasdk_registry_free};
        std::vector<std::unique_ptr<Tool>> tools;
        std::vector<std::unique_ptr<Step>> steps;
    };

    static void callTool(const char* arguments, vasdk_result* result, void* user) {
        try {
            const std::string json = (*static_cast<Tool*>(user))(arguments ? arguments : "{}");
            vasdk_result_set(result, json.c_str());
        } catch (const std::exception& exception) {
            vasdk_result_fail(result, exception.what());
        } catch (...) {
            vasdk_result_fail(result, "the tool threw");
        }
    }
    static void callStep(const char* params, vasdk_turn* turn, void* user) {
        Turn wrapped(turn);
        try {
            (*static_cast<Step*>(user))(params ? params : "{}", wrapped);
        } catch (...) {
            // a step can't fail the turn: what it did before throwing stands
        }
    }

    std::shared_ptr<State> _state;
};

/// An agent, checked and compiled
class Agent {
public:
    /// An agent file (.txtpb, .json or .pb)
    static Agent load(const std::string& path) { return Agent(vasdk_agent_load_checked(path, nullptr), nullptr); }
    static Agent load(const std::string& path, const Registry& registry) {
        return Agent(vasdk_agent_load_checked(path, registry.get()), registry._state);
    }
    /// A vasdk.flow.Agent from memory
    static Agent parse(std::string_view data, vasdk_format format, const Registry* registry = nullptr) {
        char* error = nullptr;
        vasdk_agent* agent = vasdk_agent_parse(data.data(), data.size(), format, registry ? registry->get() : nullptr, &error);
        if (!agent) detail::raise(error, "agent not parsed");
        return Agent(agent, registry ? registry->_state : nullptr);
    }
    vasdk_agent* get() const { return _agent.get(); }

private:
    friend class Conversation;
    friend class Session;
    static vasdk_agent* vasdk_agent_load_checked(const std::string& path, const vasdk_registry* registry) {
        char* error = nullptr;
        vasdk_agent* agent = vasdk_agent_load(path.c_str(), registry, &error);
        if (!agent) detail::raise(error, "agent not loaded");
        return agent;
    }
    Agent(vasdk_agent* agent, std::shared_ptr<Registry::State> registry) : _registry(std::move(registry)), _agent(agent, &vasdk_agent_free) {}

    std::shared_ptr<Registry::State> _registry;   // its callbacks outlive the agent
    detail::Shared<vasdk_agent> _agent;
};

/// One conversation with an agent, by text. Each call returns the reply as JSON. One thread at a time.
class Conversation {
public:
    explicit Conversation(const Agent& agent)
        : _agent(agent), _conversation(vasdk_conversation_create(agent.get()), &vasdk_conversation_free) {
        if (!_conversation) throw Error("conversation not created");
    }
    std::string start() { return detail::take(vasdk_conversation_start(get())); }
    std::string say(const std::string& text) { return detail::take(vasdk_conversation_say(get(), text.c_str())); }
    std::string noInput() { return detail::take(vasdk_conversation_no_input(get())); }
    std::string selectChoice(int index) { return detail::take(vasdk_conversation_select_choice(get(), index)); }
    std::string fireTrigger(const std::string& name, const std::string& payloadJson = "{}") {
        return detail::take(vasdk_conversation_fire_trigger(get(), name.c_str(), payloadJson.c_str()));
    }
    void setParam(const std::string& name, const std::string& valueJson) {
        char* error = nullptr;
        if (!vasdk_conversation_set_param(get(), name.c_str(), valueJson.c_str(), &error)) detail::raise(error, "param not set");
    }
    std::string lookup(const std::string& reference) { return detail::take(vasdk_conversation_lookup(get(), reference.c_str())); }
    vasdk_conversation* get() const { return _conversation.get(); }

private:
    Agent _agent;
    std::unique_ptr<vasdk_conversation, void (*)(vasdk_conversation*)> _conversation;
};

/// The providers an engine config can name, by id
class Providers {
public:
    /// Empty: add the app's own through get() and vasdk_providers_add_*
    Providers() : _providers(vasdk_providers_create(), &vasdk_providers_free) {}
    /// Every provider built into the library (its flavor: docs/integrating.md)
    static Providers builtIn() {
        Providers providers;
        vasdk_providers_add_all(providers.get());
        return providers;
    }
    vasdk_providers* get() const { return _providers.get(); }

private:
    friend class Session;
    friend class Runtime;
    detail::Shared<vasdk_providers> _providers;
};

/// A spoken session: an engine config's pipeline, with an agent or the LLM alone
class Session {
public:
    using EventHandler = std::function<void(std::string_view eventJson)>;
    using AudioHandler = std::function<void(const short* pcm, std::size_t samples)>;

    /// optionsJson: {"id": "call-1", "models_dir": "...", "system_prompt": "...", "format_instruction": "..."}
    Session(const std::string& configPath, const Providers& providers, const Agent* agent = nullptr,
            const std::string& optionsJson = {})
        : _providers(providers._providers), _agent(agent ? agent->_agent : nullptr), _registry(agent ? agent->_registry : nullptr),
          _handlers(std::make_unique<Handlers>()), _session(nullptr, &vasdk_session_free) {
        char* error = nullptr;
        _session.reset(vasdk_session_create(configPath.c_str(), _providers.get(), _agent.get(),
                                            optionsJson.empty() ? nullptr : optionsJson.c_str(), &error));
        if (!_session) detail::raise(error, "session not created");
    }
    Session(const std::string& configPath, const Providers& providers, const Agent& agent, const std::string& optionsJson = {})
        : Session(configPath, providers, &agent, optionsJson) {}

    /// Before start(): each vasdk.engine.Event as JSON, on the session's event thread
    void onEvent(EventHandler handler) {
        _handlers->event = std::move(handler);
        vasdk_session_on_event(get(), [](const char* json, void* user) {
            auto& handler = static_cast<Handlers*>(user)->event;
            if (handler) handler(json ? json : "");
        }, _handlers.get());
    }
    /// The assistant's speech at outputRate(), on a pipeline thread
    void onAudio(AudioHandler handler) {
        _handlers->audio = std::move(handler);
        vasdk_session_on_audio(get(), [](const short* pcm, std::size_t samples, void* user) {
            auto& handler = static_cast<Handlers*>(user)->audio;
            if (handler) handler(pcm, samples);
        }, _handlers.get());
    }
    void start() {
        char* error = nullptr;
        if (!vasdk_session_start(get(), &error)) detail::raise(error, "session not started");
    }
    void stop() { vasdk_session_stop(get()); }
    int inputRate() const { return vasdk_session_input_rate(get()); }
    int outputRate() const { return vasdk_session_output_rate(get()); }
    void pushAudio(const short* pcm, std::size_t samples) { vasdk_session_push_audio(get(), pcm, samples); }
    void injectText(const std::string& text) { vasdk_session_inject_text(get(), text.c_str()); }
    void say(const std::string& text) { vasdk_session_say(get(), text.c_str()); }
    void setParam(const std::string& name, const std::string& valueJson) {
        char* error = nullptr;
        if (!vasdk_session_set_param(get(), name.c_str(), valueJson.c_str(), &error)) detail::raise(error, "param not set");
    }
    void fireTrigger(const std::string& name, const std::string& payloadJson = "{}") {
        char* error = nullptr;
        if (!vasdk_session_fire_trigger(get(), name.c_str(), payloadJson.c_str(), &error)) detail::raise(error, "trigger not fired");
    }
    void setContext(const std::string& contextJson) {
        char* error = nullptr;
        if (!vasdk_session_set_context(get(), contextJson.c_str(), &error)) detail::raise(error, "context not set");
    }
    std::string snapshot() { return detail::take(vasdk_session_snapshot(get())); }
    void resume(const std::string& snapshotJson) { vasdk_session_resume(get(), snapshotJson.c_str()); }
    std::string describe() { return detail::take(vasdk_session_describe(get())); }
    vasdk_session* get() const { return _session.get(); }

private:
    friend class Runtime;
    struct Handlers {
        EventHandler event;
        AudioHandler audio;
    };
    Session(vasdk_session* session, detail::Shared<vasdk_providers> providers, const Agent* agent, std::shared_ptr<void> runtime)
        : _providers(std::move(providers)), _agent(agent ? agent->_agent : nullptr), _registry(agent ? agent->_registry : nullptr),
          _runtime(std::move(runtime)), _handlers(std::make_unique<Handlers>()), _session(session, &vasdk_session_free) {}

    // What the C session uses, alive until it is freed (members go in reverse: the session first)
    detail::Shared<vasdk_providers> _providers;
    detail::Shared<vasdk_agent> _agent;
    std::shared_ptr<Registry::State> _registry;
    std::shared_ptr<void> _runtime;
    std::unique_ptr<Handlers> _handlers;
    std::unique_ptr<vasdk_session, void (*)(vasdk_session*)> _session;
};

/// Providers loaded once and lent to one session after another (a server)
class Runtime {
public:
    Runtime(const std::string& configPath, const Providers& providers, const std::string& modelsDir = {}, int maxSessions = 0)
        : _providers(providers._providers) {
        char* error = nullptr;
        vasdk_runtime* runtime = vasdk_runtime_create(configPath.c_str(), _providers.get(), modelsDir.empty() ? nullptr : modelsDir.c_str(),
                                                      maxSessions, &error);
        if (!runtime) detail::raise(error, "runtime not created");
        _runtime = std::shared_ptr<vasdk_runtime>(runtime, &vasdk_runtime_free);
    }
    /// A session over the runtime's providers; at maxSessions it throws
    Session session(const Agent* agent = nullptr) {
        char* error = nullptr;
        vasdk_session* session = vasdk_runtime_session_create(get(), agent ? agent->get() : nullptr, nullptr, 0, VASDK_FORMAT_JSON, &error);
        if (!session) detail::raise(error, "session not created");
        return Session(session, _providers, agent, _runtime);
    }
    std::size_t activeSessions() const { return vasdk_runtime_active_sessions(get()); }
    std::size_t idleProviderSets() const { return vasdk_runtime_idle_provider_sets(get()); }
    vasdk_runtime* get() const { return _runtime.get(); }

private:
    detail::Shared<vasdk_providers> _providers;
    std::shared_ptr<vasdk_runtime> _runtime;   // sessions hold it too: freed after the last one
};

/// The model store (docs/models.md): downloads, checks and finds the files local providers load
class Models {
public:
    /// Bytes of `model` in the store so far, of `total`; `file` downloading. Return false to stop.
    using Progress = std::function<bool(std::string_view model, std::string_view file, std::uint64_t done, std::uint64_t total)>;

    /// optionsJson: {"directory", "manifest_path", "token", "offline", "architecture"} (vasdk_models_create)
    explicit Models(const std::string& optionsJson = {}) : _models(nullptr, &vasdk_models_free) {
        char* error = nullptr;
        _models.reset(vasdk_models_create(optionsJson.empty() ? nullptr : optionsJson.c_str(), &error));
        if (!_models) detail::raise(error, "model store not created");
    }
    /// Where sessions look when models_dir is unset
    static std::string defaultDirectory() { return detail::take(vasdk_models_default_directory()); }
    std::string directory() const { return vasdk_models_directory(get()); }
    /// Every model of this machine, with its status, as JSON (vasdk_models_list)
    std::string list() const { return detail::take(vasdk_models_list(get())); }
    /// The ids of the models an engine config's providers load
    std::vector<std::string> forConfig(const std::string& configPath) const {
        char* error = nullptr;
        char* ids = vasdk_models_for_config(get(), configPath.c_str(), &error);
        if (!ids) detail::raise(error, "config not loaded");
        return detail::stringArray(detail::take(ids));
    }
    vasdk_model_status status(const std::string& id) const { return vasdk_models_status(get(), id.c_str()); }
    /// Downloads what is missing; acceptLicense for a model whose license needs reading first
    void pull(const std::string& id, bool acceptLicense = false, Progress progress = {}) const {
        char* error = nullptr;
        const int pulled = vasdk_models_pull(get(), id.c_str(), acceptLicense ? 1 : 0, progress ? &callProgress : nullptr,
                                             progress ? &progress : nullptr, &error);
        if (!pulled) detail::raise(error, "model not pulled");
    }
    /// Hashes the model's files; throws when one is missing or doesn't match
    void verify(const std::string& id) const {
        char* error = nullptr;
        if (!vasdk_models_verify(get(), id.c_str(), &error)) detail::raise(error, "model not verified");
    }
    void remove(const std::string& id) const {
        char* error = nullptr;
        if (!vasdk_models_remove(get(), id.c_str(), &error)) detail::raise(error, "model not removed");
    }
    vasdk_models* get() const { return _models.get(); }

private:
    static int callProgress(const char* model, const char* file, unsigned long long done, unsigned long long total, void* user) {
        try {
            return (*static_cast<Progress*>(user))(model ? model : "", file ? file : "", done, total) ? 1 : 0;
        } catch (...) {
            return 0;
        }
    }
    std::unique_ptr<vasdk_models, void (*)(vasdk_models*)> _models;
};

} // namespace vasdk

#endif // VASDK_C_VASDK_HPP
