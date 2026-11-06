// The C API's spoken sessions (VASDK/c/vasdk.h): a VAEngine whose audio the app brings.

#include "c/vasdk.h"
#include "c/vasdk_cpp.h"
#include "vasdk_c_internal.h"
#include "context/ContextProvider.h"

#include "../AudioFrameUtil.h"
#include "Transport.h"
#include "VAEngine.h"
#include "engine/ProtoFileLoader.h"
#include "engine/Runtime.h"
#include "vasdk/engine/engine_config.pb.h"
#include "vasdk/engine/events.pb.h"
#include "vasdk/engine/session.pb.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct vasdk_providers {
    va::engine::ProviderRegistry registry;
};

namespace {

char* copyText(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

template <class Result>
Result failWith(char** error, const std::string& message, Result result) {
    if (error) *error = copyText(message);
    return result;
}

// Audio the app pushes in (into the ring buffer, a frame at a time) and speech it gets back
// through a callback: a phone line, a file, its own device.
class PushAudioTransport : public va::transport::AudioTransport {
public:
    std::string getName() const override { return "AppAudio"; }
    std::string getDescription() const override { return "audio pushed in by the app, speech given back to it"; }
    bool initialize() override { return true; }
    bool start() override {
        setState(va::transport::TransportState::CONNECTED);
        return true;
    }
    void stop() override { setState(va::transport::TransportState::DISCONNECTED); }
    void release() override {}
    void setInputAudioConfig(const va::audio::AudioConfig& config) override { _input = config; }
    void setOutputAudioConfig(const va::audio::AudioConfig& config) override { _output = config; }
    void setFrameSize(size_t samples) override { _frameSize = samples; }
    void setRingBuffer(std::shared_ptr<va::AudioRingBuffer> buffer) override { _ringBuffer = std::move(buffer); }

    bool sendFrame(std::shared_ptr<va::transport::Frame> frame) override {
        if (!frame || frame->getType() != va::transport::FrameType::SYNTHESIS) return true;
        auto speech = std::dynamic_pointer_cast<va::transport::SynthesisFrame>(frame);
        if (!speech || speech->audioData.empty() || !onAudio) return true;
        onAudio(reinterpret_cast<const short*>(speech->audioData.data()), speech->audioData.size() / sizeof(short), audioUser);
        return true;
    }

    // Splits what comes into the pipeline's frames; a part frame waits for the rest.
    void push(const short* pcm, size_t samples) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_ringBuffer || _frameSize == 0) return;
        _pending.insert(_pending.end(), pcm, pcm + samples);
        size_t at = 0;
        while (_pending.size() - at >= _frameSize) {
            auto audio = va::transport::detail::writeAudioFrame(
                _ringBuffer.get(), reinterpret_cast<const char*>(_pending.data() + at), static_cast<uint32_t>(_frameSize), _input,
                _absoluteOffset, _sequence++, getName());
            at += _frameSize;
            if (onFrameReceived) onFrameReceived(audio);
        }
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<long>(at));
    }

    vasdk_audio_fn onAudio = nullptr;
    void* audioUser = nullptr;

private:
    std::mutex _mutex;
    va::audio::AudioConfig _input{}, _output{};
    size_t _frameSize = 0;
    std::shared_ptr<va::AudioRingBuffer> _ringBuffer;
    std::vector<short> _pending;
    uint64_t _absoluteOffset = 0;
    uint64_t _sequence = 0;
};

} // namespace

struct vasdk_session {
    std::shared_ptr<PushAudioTransport> transport;
    std::shared_ptr<va::context::ContextProvider> context;   // vasdk_session_set_context
    std::unique_ptr<va::VAEngine> engine;
    int inputRate = 16000;
    int outputRate = 16000;
    vasdk_event_fn onEvent = nullptr;          // events as JSON
    vasdk_message_fn onEventMessage = nullptr; // or as messages, in eventFormat
    vasdk_format eventFormat = VASDK_FORMAT_BINARY;
    void* eventUser = nullptr;
};

namespace {

vasdk_session* createSession(const char* config_path, const vasdk_providers* providers, const vasdk_agent* agent,
                             const vasdk::engine::SessionOptions& given, char** error);

// A tool for the session's LLM, its callback the app's.
va::llm::Tool appTool(const std::string& name, const std::string& description, vasdk_tool_fn tool, void* user) {
    va::llm::Tool added(name, description);
    added.onCall([tool, user, name](const nlohmann::json& args) {
        vasdk_result result;
        tool(args.dump().c_str(), &result, user);
        if (result.failed) return nlohmann::json{{"error", result.error.empty() ? name + " failed" : result.error}};
        if (result.json.empty()) return nlohmann::json::object();
        auto parsed = nlohmann::json::parse(result.json, nullptr, false);
        return parsed.is_discarded() ? nlohmann::json{{"result", result.json}} : parsed;
    });
    return added;
}

nlohmann::json structJson(const google::protobuf::Struct& value) {
    std::string json;
    if (!google::protobuf::util::MessageToJsonString(value, &json).ok()) return nlohmann::json::object();
    return nlohmann::json::parse(json, nullptr, false);
}

} // namespace

va::engine::ProviderRegistry& vasdk_providers_registry(vasdk_providers* providers) { return providers->registry; }

extern "C" {

vasdk_providers* vasdk_providers_create(void) { return new vasdk_providers(); }
void vasdk_providers_free(vasdk_providers* providers) { delete providers; }

vasdk_session* vasdk_session_create(const char* config_path, const vasdk_providers* providers, const vasdk_agent* agent,
                                    const char* options_json, char** error) {
    if (!config_path || !providers) return failWith(error, "no config path or providers", static_cast<vasdk_session*>(nullptr));
    vasdk::engine::SessionOptions options;
    if (options_json && *options_json) {
        const auto given = nlohmann::json::parse(options_json, nullptr, false);
        if (!given.is_object()) return failWith(error, "options: not a JSON object", static_cast<vasdk_session*>(nullptr));
        options.set_id(given.value("id", std::string()));
        options.set_models_dir(given.value("models_dir", std::string()));
        options.set_system_prompt(given.value("system_prompt", std::string()));
        options.set_format_instruction(given.value("format_instruction", std::string()));
    }
    return createSession(config_path, providers, agent, options, error);
}

vasdk_session* vasdk_session_create_message(const char* config_path, const vasdk_providers* providers, const vasdk_agent* agent,
                                            const void* options, size_t length, vasdk_format format, char** error) {
    if (!config_path || !providers) return failWith(error, "no config path or providers", static_cast<vasdk_session*>(nullptr));
    vasdk::engine::SessionOptions parsed;
    std::string reason;
    if (!vasdk_c::read(options, length, format, parsed, &reason))
        return failWith(error, "SessionOptions: " + reason, static_cast<vasdk_session*>(nullptr));
    return createSession(config_path, providers, agent, parsed, error);
}

} // extern "C"

namespace {

vasdk_session* createSession(const char* config_path, const vasdk_providers* providers, const vasdk_agent* agent,
                             const vasdk::engine::SessionOptions& given, char** error) {
    vasdk::engine::EngineConfig config;
    std::string reason;
    if (!va::engine::loadEngineConfig(config_path, config, &reason))
        return failWith(error, reason, static_cast<vasdk_session*>(nullptr));

    va::VAEngine::Options options;
    options.id = given.id();
    options.modelsDir = given.models_dir();
    options.systemPrompt = given.system_prompt();
    options.responseFormatInstruction = given.format_instruction();
    if (!va::engine::applyAudioConfig(config.audio(), options.inputAudio, options.outputAudio, options.frameSize, &reason))
        return failWith(error, std::string(config_path) + ": " + reason, static_cast<vasdk_session*>(nullptr));
    if (options.inputAudio.format != va::audio::AudioFormat::SHORT || options.outputAudio.format != va::audio::AudioFormat::SHORT ||
        options.inputAudio.channels != 1 || options.outputAudio.channels != 1)
        return failWith(error, std::string(config_path) + ": audio: the C API takes and gives 16-bit mono PCM",
                        static_cast<vasdk_session*>(nullptr));

    auto session = std::make_unique<vasdk_session>();
    session->transport = std::make_shared<PushAudioTransport>();
    session->inputRate = static_cast<int>(options.inputAudio.sampleRate);
    session->outputRate = static_cast<int>(options.outputAudio.sampleRate);
    options.transport = session->transport;
    session->context = std::make_shared<va::context::ContextProvider>();
    options.context = session->context;
    if (agent) options.agent = vasdk_agent_compiled(agent);
    session->engine = va::VAEngine::create(config, providers->registry, options, &reason);
    if (!session->engine) return failWith(error, reason, static_cast<vasdk_session*>(nullptr));
    auto* raw = session.get();
    session->engine->addListener([raw](const vasdk::engine::Event& event) {
        if (raw->onEventMessage) {
            const std::string data = vasdk_c::written(event, raw->eventFormat);
            raw->onEventMessage(data.data(), data.size(), raw->eventUser);
            return;
        }
        if (!raw->onEvent) return;
        std::string json;
        if (google::protobuf::util::MessageToJsonString(event, &json).ok()) raw->onEvent(json.c_str(), raw->eventUser);
    });
    return session.release();
}

} // namespace

extern "C" {

void vasdk_session_free(vasdk_session* session) {
    if (!session) return;
    if (session->engine) session->engine->stop();
    delete session;
}

void vasdk_session_on_event(vasdk_session* session, vasdk_event_fn on_event, void* user) {
    if (!session) return;
    session->onEvent = on_event;
    session->onEventMessage = nullptr;
    session->eventUser = user;
}

void vasdk_session_on_event_message(vasdk_session* session, vasdk_message_fn on_event, vasdk_format format, void* user) {
    if (!session) return;
    session->onEventMessage = on_event;
    session->onEvent = nullptr;
    session->eventFormat = format;
    session->eventUser = user;
}

void vasdk_session_on_audio(vasdk_session* session, vasdk_audio_fn on_audio, void* user) {
    if (!session) return;
    session->transport->onAudio = on_audio;
    session->transport->audioUser = user;
}

void vasdk_session_resume(vasdk_session* session, const char* snapshot_json) {
    if (session && snapshot_json && *snapshot_json) session->engine->resumeConversation(snapshot_json);
}

int vasdk_session_start(vasdk_session* session, char** error) {
    if (!session) return failWith(error, "no session", 0);
    std::string reason;
    if (!session->engine->start(&reason)) return failWith(error, reason, 0);
    return 1;
}

void vasdk_session_stop(vasdk_session* session) {
    if (session) session->engine->stop();
}

int vasdk_session_input_rate(const vasdk_session* session) { return session ? session->inputRate : 0; }
int vasdk_session_output_rate(const vasdk_session* session) { return session ? session->outputRate : 0; }

void vasdk_session_push_audio(vasdk_session* session, const short* pcm, size_t samples) {
    if (session && pcm && samples > 0) session->transport->push(pcm, samples);
}

void vasdk_session_inject_text(vasdk_session* session, const char* text) {
    if (session && text) session->engine->injectUserText(text);
}

void vasdk_session_say(vasdk_session* session, const char* text) {
    if (session && text) session->engine->say(text);
}

int vasdk_session_set_param(vasdk_session* session, const char* name, const char* value_json, char** error) {
    if (!session || !name || !value_json) return failWith(error, "no session, name or value", 0);
    google::protobuf::Value value;
    if (!google::protobuf::util::JsonStringToMessage(value_json, &value).ok())
        return failWith(error, std::string("not a JSON value: ") + value_json, 0);
    session->engine->setSessionParam(name, value);
    return 1;
}

int vasdk_session_send(vasdk_session* session, const void* command, size_t length, vasdk_format format, char** error) {
    if (!session) return failWith(error, "no session", 0);
    vasdk::engine::Command parsed;
    std::string reason;
    if (!vasdk_c::read(command, length, format, parsed, &reason)) return failWith(error, "Command: " + reason, 0);
    if (!session->engine->handle(parsed, &reason)) return failWith(error, reason, 0);
    return 1;
}

void vasdk_session_set_network_available(vasdk_session* session, int available) {
    if (session) session->engine->setNetworkAvailable(available != 0);
}

char* vasdk_session_snapshot(vasdk_session* session) {
    return copyText(session ? session->engine->conversationSnapshot() : std::string());
}

int vasdk_session_add_tool(vasdk_session* session, const char* name, const char* description, const char* parameters_json,
                           vasdk_tool_fn tool, void* user, char** error) {
    if (!session || !name || !*name || !tool) return failWith(error, "no session, name or tool", 0);
    va::llm::Tool added = appTool(name, description ? description : "", tool, user);
    if (parameters_json && *parameters_json) {
        const auto parameters = nlohmann::json::parse(parameters_json, nullptr, false);
        if (!parameters.is_array()) return failWith(error, std::string("parameters: not a JSON array: ") + parameters_json, 0);
        for (const auto& parameter : parameters) {
            if (!parameter.is_object() || !parameter.contains("name") || !parameter["name"].is_string())
                return failWith(error, "parameters: each needs a \"name\": " + parameter.dump(), 0);
            added.addParameter({parameter["name"].get<std::string>(), parameter.value("description", std::string()),
                                parameter.value("type", std::string("string")), parameter.value("required", true)});
        }
    }
    session->engine->registerTool(added);
    return 1;
}

int vasdk_session_add_tool_message(vasdk_session* session, const void* declaration, size_t length, vasdk_format format,
                                   vasdk_tool_fn tool, void* user, char** error) {
    if (!session || !tool) return failWith(error, "no session or tool", 0);
    vasdk::engine::ToolDeclaration parsed;
    std::string reason;
    if (!vasdk_c::read(declaration, length, format, parsed, &reason)) return failWith(error, "ToolDeclaration: " + reason, 0);
    if (parsed.name().empty()) return failWith(error, "ToolDeclaration: no name", 0);
    va::llm::Tool added = appTool(parsed.name(), parsed.description(), tool, user);
    for (const auto& parameter : parsed.parameters()) {
        if (parameter.name().empty()) return failWith(error, "ToolDeclaration " + parsed.name() + ": a parameter without a name", 0);
        added.addParameter({parameter.name(), parameter.description(), parameter.type().empty() ? std::string("string") : parameter.type(),
                            !parameter.has_required() || parameter.required()});
    }
    session->engine->registerTool(added);
    return 1;
}

int vasdk_session_fire_trigger(vasdk_session* session, const char* name, const char* payload_json, char** error) {
    if (!session || !name || !*name) return failWith(error, "no session or trigger name", 0);
    google::protobuf::Struct payload;
    if (payload_json && *payload_json && !google::protobuf::util::JsonStringToMessage(payload_json, &payload).ok())
        return failWith(error, std::string("payload: not a JSON object: ") + payload_json, 0);
    session->engine->fireTrigger(name, payload);
    return 1;
}

int vasdk_session_set_context(vasdk_session* session, const char* context_json, char** error) {
    if (!session || !context_json) return failWith(error, "no session or context", 0);
    const auto context = nlohmann::json::parse(context_json, nullptr, false);
    if (!context.is_object()) return failWith(error, std::string("context: not a JSON object: ") + context_json, 0);
    for (const auto& [key, value] : context.items()) session->context->setValue(key, value);
    return 1;
}

int vasdk_session_set_context_message(vasdk_session* session, const void* context, size_t length, vasdk_format format,
                                      char** error) {
    if (!session) return failWith(error, "no session", 0);
    google::protobuf::Struct parsed;
    std::string reason;
    if (!vasdk_c::read(context, length, format, parsed, &reason)) return failWith(error, "Struct: " + reason, 0);
    const auto values = structJson(parsed);
    for (const auto& [key, value] : values.items()) session->context->setValue(key, value);
    return 1;
}

} // extern "C"
