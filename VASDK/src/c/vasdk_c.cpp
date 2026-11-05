// The C API (VASDK/c/vasdk.h) over FlowRegistry, CompiledAgent, FlowRuntime and FlowTestRunner.

#include "c/vasdk.h"
#include "c/vasdk_cpp.h"

#include "engine/ProtoFileLoader.h"
#include "flow/CompiledAgent.h"
#include "flow/FlowRegistry.h"
#include "flow/FlowRuntime.h"
#include "flow/FlowTestRunner.h"
#include "vasdk/flow/step_outcome.pb.h"

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifndef VASDK_C_VERSION
#define VASDK_C_VERSION "0"
#endif

struct vasdk_registry {
    std::shared_ptr<va::flow::FlowRegistry> registry = std::make_shared<va::flow::FlowRegistry>();
};
struct vasdk_agent {
    std::shared_ptr<const va::flow::CompiledAgent> agent;
};
struct vasdk_conversation {
    explicit vasdk_conversation(std::shared_ptr<const va::flow::CompiledAgent> agent) : runtime(std::move(agent)) {}
    va::flow::FlowRuntime runtime;
};
struct vasdk_turn {
    va::flow::TurnContext& context;
};
struct vasdk_result {
    std::string json;
    std::string error;
    bool failed = false;
};

namespace {

char* copy(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

template <class Result>
Result fail(char** error, const std::string& message, Result result) {
    if (error) *error = copy(message);
    return result;
}

std::string toJson(const google::protobuf::Message& message) {
    std::string json;
    if (!google::protobuf::util::MessageToJsonString(message, &json).ok()) return "null";
    return json;
}

va::engine::ProtoFormat formatOf(vasdk_format format) {
    switch (format) {
    case VASDK_FORMAT_JSON: return va::engine::ProtoFormat::Json;
    case VASDK_FORMAT_BINARY: return va::engine::ProtoFormat::Binary;
    case VASDK_FORMAT_TEXT: break;
    }
    return va::engine::ProtoFormat::Text;
}

nlohmann::json parsedOr(const std::string& json, nlohmann::json fallback) {
    auto parsed = nlohmann::json::parse(json, nullptr, false);
    return parsed.is_discarded() ? fallback : parsed;
}

char* replyJson(const va::flow::Reply& reply, const va::flow::FlowRuntime& runtime) {
    nlohmann::json out{{"text", reply.text}, {"matched", reply.matched}, {"intent", reply.intent}, {"event", reply.event},
                       {"expects_reply", reply.expectsReply}, {"ends_session", reply.endsSession},
                       {"flow", runtime.flowName()}, {"page", runtime.pageName()}};
    auto& segments = out["segments"] = nlohmann::json::array();
    for (const auto& segment : reply.segments) segments.push_back({{"text", segment.text}, {"interruptible", segment.interruptible}});
    auto& choices = out["choices"] = nlohmann::json::array();
    for (const auto& choice : reply.choices) choices.push_back({{"label", choice.label}, {"value", choice.value}});
    auto& tools = out["tool_calls"] = nlohmann::json::array();
    for (const auto& call : reply.toolCalls)
        tools.push_back({{"name", call.name}, {"arguments", parsedOr(call.argumentsJson, nullptr)},
                         {"result", parsedOr(call.resultJson, nullptr)}, {"ok", call.ok}});
    auto& triggers = out["triggers"] = nlohmann::json::array();
    for (const auto& trigger : reply.triggers)
        triggers.push_back({{"name", trigger.name}, {"decision", vasdk::flow::TriggerDecision_Name(trigger.decision)}});
    return copy(out.dump());
}

std::string valueJson(const va::flow::Value& value) { return toJson(value); }

} // namespace

extern "C" {

void vasdk_free(void* text) { std::free(text); }
const char* vasdk_version(void) { return VASDK_C_VERSION; }

//  Registry

vasdk_registry* vasdk_registry_create(void) { return new vasdk_registry(); }
void vasdk_registry_free(vasdk_registry* registry) { delete registry; }

int vasdk_registry_add_step(vasdk_registry* registry, const char* params_type, vasdk_step_fn step, void* user) {
    if (!registry || !params_type || !step) return 0;
    registry->registry->addStep(params_type, [step, user](const google::protobuf::Message& params, va::flow::TurnContext& context) {
        vasdk_turn turn{context};
        step(toJson(params).c_str(), &turn, user);
    });
    return 1;
}

int vasdk_registry_add_tool(vasdk_registry* registry, const char* name, const char* description, vasdk_tool_fn tool, void* user) {
    if (!registry || !name || !tool) return 0;
    va::llm::Tool entry(name, description ? description : "");
    entry.onCall([tool, user, toolName = std::string(name)](const nlohmann::json& args) -> nlohmann::json {
        vasdk_result result;
        tool(args.dump().c_str(), &result, user);
        if (result.failed) return {{"error", result.error.empty() ? "failed" : result.error}};
        auto value = nlohmann::json::parse(result.json.empty() ? "null" : result.json, nullptr, false);
        if (value.is_discarded()) return {{"error", "tool " + toolName + " returned something that isn't JSON"}};
        return value;
    });
    registry->registry->addTool(std::move(entry));
    return 1;
}

void vasdk_result_set(vasdk_result* result, const char* json) {
    if (!result) return;
    result->json = json ? json : "null";
    result->failed = false;
}

void vasdk_result_fail(vasdk_result* result, const char* error) {
    if (!result) return;
    result->error = error ? error : "failed";
    result->failed = true;
}

int vasdk_registry_add_envelope(vasdk_registry* registry, const char* name, vasdk_envelope_before_fn before,
                                vasdk_envelope_after_fn after, void* user) {
    if (!registry || !name) return 0;
    va::flow::FlowRegistry::Envelope envelope;
    if (before)
        envelope.before = [before, user](va::flow::TurnContext& context) {
            vasdk_turn turn{context};
            return before(&turn, user) != 0;
        };
    if (after)
        envelope.after = [after, user](va::flow::TurnContext& context) {
            vasdk_turn turn{context};
            after(&turn, user);
        };
    registry->registry->addEnvelope(name, std::move(envelope));
    return 1;
}

//  Turn

const char* vasdk_turn_text(vasdk_turn* turn) { return turn ? turn->context.text().c_str() : ""; }

char* vasdk_turn_lookup(vasdk_turn* turn, const char* reference) {
    if (!turn || !reference) return copy("null");
    return copy(valueJson(turn->context.lookup(reference)));
}

void vasdk_turn_say(vasdk_turn* turn, const char* text) {
    if (turn && text) turn->context.say(text);
}

void vasdk_turn_goto(vasdk_turn* turn, const char* target) {
    if (turn && target) turn->context.goTo(target);
}

int vasdk_turn_apply(vasdk_turn* turn, const void* outcome, size_t length, vasdk_format format, char** error) {
    if (!turn || !outcome) return fail(error, "no turn or outcome", 0);
    vasdk::flow::StepOutcome parsed;
    std::string reason;
    if (!va::engine::parseProto(std::string(static_cast<const char*>(outcome), length), formatOf(format), parsed, &reason))
        return fail(error, "StepOutcome: " + reason, 0);
    if (!turn->context.apply(parsed, &reason)) return fail(error, reason, 0);
    return 1;
}

//  Agents

vasdk_agent* vasdk_agent_load(const char* path, const vasdk_registry* registry, char** error) {
    if (!path) return fail(error, "no path", static_cast<vasdk_agent*>(nullptr));
    std::string reason;
    auto agent = va::flow::CompiledAgent::load(path, &reason, nullptr, registry ? registry->registry : nullptr);
    if (!agent) return fail(error, reason, static_cast<vasdk_agent*>(nullptr));
    return new vasdk_agent{std::move(agent)};
}

vasdk_agent* vasdk_agent_parse(const void* data, size_t length, vasdk_format format, const vasdk_registry* registry,
                               char** error) {
    if (!data) return fail(error, "no data", static_cast<vasdk_agent*>(nullptr));
    vasdk::flow::Agent definition;
    std::string reason;
    if (!va::engine::parseProto(std::string(static_cast<const char*>(data), length), formatOf(format), definition, &reason))
        return fail(error, reason, static_cast<vasdk_agent*>(nullptr));
    auto agent = va::flow::CompiledAgent::compile(std::move(definition), &reason, nullptr, registry ? registry->registry : nullptr);
    if (!agent) return fail(error, reason, static_cast<vasdk_agent*>(nullptr));
    return new vasdk_agent{std::move(agent)};
}

void vasdk_agent_free(vasdk_agent* agent) { delete agent; }

//  Conversations

vasdk_conversation* vasdk_conversation_create(const vasdk_agent* agent) {
    return agent ? new vasdk_conversation(agent->agent) : nullptr;
}

void vasdk_conversation_free(vasdk_conversation* conversation) { delete conversation; }

char* vasdk_conversation_start(vasdk_conversation* conversation) {
    if (!conversation) return nullptr;
    return replyJson(conversation->runtime.start(), conversation->runtime);
}

char* vasdk_conversation_say(vasdk_conversation* conversation, const char* text) {
    if (!conversation) return nullptr;
    return replyJson(conversation->runtime.handleText(text ? text : ""), conversation->runtime);
}

char* vasdk_conversation_no_input(vasdk_conversation* conversation) {
    if (!conversation) return nullptr;
    return replyJson(conversation->runtime.handleNoInput(), conversation->runtime);
}

char* vasdk_conversation_select_choice(vasdk_conversation* conversation, int index) {
    if (!conversation) return nullptr;
    return replyJson(conversation->runtime.selectChoice(index), conversation->runtime);
}

char* vasdk_conversation_fire_trigger(vasdk_conversation* conversation, const char* name, const char* payload_json) {
    if (!conversation || !name) return nullptr;
    google::protobuf::Struct payload;
    if (payload_json && *payload_json && !google::protobuf::util::JsonStringToMessage(payload_json, &payload).ok())
        spdlog::warn("[vasdk_c] fire_trigger {}: the payload isn't a JSON object", name);
    return replyJson(conversation->runtime.fireTrigger(name, payload), conversation->runtime);
}

int vasdk_conversation_set_param(vasdk_conversation* conversation, const char* name, const char* value_json, char** error) {
    if (!conversation || !name || !value_json) return fail(error, "no conversation, name or value", 0);
    va::flow::Value value;
    if (!google::protobuf::util::JsonStringToMessage(value_json, &value).ok())
        return fail(error, std::string("not a JSON value: ") + value_json, 0);
    std::string reason;
    if (!conversation->runtime.setSessionParam(name, value, &reason)) return fail(error, reason, 0);
    return 1;
}

char* vasdk_conversation_lookup(vasdk_conversation* conversation, const char* reference) {
    if (!conversation || !reference) return copy("null");
    auto expression = va::flow::Expression::parse(reference);
    const va::flow::Path* path = expression ? expression->reference() : nullptr;
    if (!path) return copy("null");
    return copy(valueJson(conversation->runtime.lookup(*path)));
}

//  Tests

int vasdk_run_test_file(const vasdk_agent* agent, const char* path, char** report) {
    if (!agent || !path) return fail(report, "no agent or path", 0);
    const auto result = va::flow::runFlowTestFile(agent->agent, path);
    if (report) {
        std::string lines;
        for (const auto& failure : result.failures) lines += failure + "\n";
        *report = copy(lines);
    }
    return result.passed() ? 1 : 0;
}

} // extern "C"

std::shared_ptr<const va::flow::CompiledAgent> vasdk_agent_compiled(const vasdk_agent* agent) {
    return agent ? agent->agent : nullptr;
}
