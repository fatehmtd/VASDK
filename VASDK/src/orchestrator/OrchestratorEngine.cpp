#include <VASDK/orchestrator/OrchestratorEngine.h>

#include <VASDK/Frame.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace va {
namespace orchestrator {

namespace {

std::string generateSessionId() {
    static std::atomic<uint64_t> counter{0};
    return "session-" + std::to_string(counter.fetch_add(1) + 1);
}

int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string numberToDisplayString(double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e15) {
        return std::to_string(static_cast<long long>(d));
    }
    std::ostringstream oss;
    oss << d;
    return oss.str();
}

std::string valueToDisplayString(const google::protobuf::Value& value) {
    switch (value.kind_case()) {
    case google::protobuf::Value::kStringValue: return value.string_value();
    case google::protobuf::Value::kNumberValue: return numberToDisplayString(value.number_value());
    case google::protobuf::Value::kBoolValue:   return value.bool_value() ? "true" : "false";
    case google::protobuf::Value::kNullValue:   return "";
    default:                                    return ""; // struct/list - not expected in say-text substitution
    }
}

// Milestone 2.4 - a generative node's LLM-tool loop crosses the JSON/Struct
// boundary in both directions (a tool call's args in, a tool handler's result
// out); a plain route's CustomToolAction never needs these, since its
// args_cel already evaluates directly into Struct fields. Built from plain
// generated accessors only, deliberately never
// google::protobuf::util::JsonStringToMessage/MessageToJsonString - see
// LLMNLUProvider.h's class comment for why that reflection-based path is
// fragile once more than one statically-linked libprotobuf copy exists in
// the same process.
void jsonValueToProtoValue(const nlohmann::json& j, google::protobuf::Value& out) {
    if (j.is_string()) {
        out.set_string_value(j.get<std::string>());
    } else if (j.is_number()) {
        out.set_number_value(j.get<double>());
    } else if (j.is_boolean()) {
        out.set_bool_value(j.get<bool>());
    } else if (j.is_object()) {
        auto* s = out.mutable_struct_value();
        for (const auto& [key, value] : j.items()) {
            jsonValueToProtoValue(value, (*s->mutable_fields())[key]);
        }
    } else if (j.is_array()) {
        auto* l = out.mutable_list_value();
        for (const auto& value : j) {
            jsonValueToProtoValue(value, *l->add_values());
        }
    } else {
        out.set_null_value(google::protobuf::NULL_VALUE);
    }
}

google::protobuf::Struct jsonObjectToStruct(const nlohmann::json& j) {
    google::protobuf::Struct s;
    if (j.is_object()) {
        for (const auto& [key, value] : j.items()) {
            jsonValueToProtoValue(value, (*s.mutable_fields())[key]);
        }
    }
    return s;
}

nlohmann::json protoValueToJson(const google::protobuf::Value& v) {
    switch (v.kind_case()) {
    case google::protobuf::Value::kStringValue: return v.string_value();
    case google::protobuf::Value::kNumberValue: return v.number_value();
    case google::protobuf::Value::kBoolValue:   return v.bool_value();
    case google::protobuf::Value::kStructValue: {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [key, value] : v.struct_value().fields()) {
            obj[key] = protoValueToJson(value);
        }
        return obj;
    }
    case google::protobuf::Value::kListValue: {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& value : v.list_value().values()) {
            arr.push_back(protoValueToJson(value));
        }
        return arr;
    }
    case google::protobuf::Value::kNullValue:
    default:
        return nullptr;
    }
}

nlohmann::json structToJson(const google::protobuf::Struct& s) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [key, value] : s.fields()) {
        obj[key] = protoValueToJson(value);
    }
    return obj;
}

// Milestone 2.4 (spec §9.4) - a generative node's tools list is name+terminal
// only (GenerativeConfig.ToolRef carries no description - see domain.proto),
// so this is also how runGenerativeTurn enforces "the LLM cannot see or call
// anything outside that list": a tool name absent here is refused even if a
// customTool handler happens to be registered under that name for other
// nodes/routes.
const GenerativeConfig::ToolRef* findGenerativeToolRef(const GenerativeConfig& config, const std::string& name) {
    for (const auto& tool : config.tools()) {
        if (tool.name() == name) return &tool;
    }
    return nullptr;
}

} // namespace

OrchestratorEngine::OrchestratorEngine(va::orchestrator::Agent agent, OrchestratorConfig config)
    : _agent(std::move(agent))
    , _config(config)
    , _session(generateSessionId(), "", "") {
    compileAllExpressions();
    initializeMetrics();
}

OrchestratorEngine::OrchestratorEngine(va::orchestrator::Agent agent, const SessionSnapshot& snapshot,
                                       OrchestratorConfig config)
    : _agent(std::move(agent))
    , _config(config)
    , _session(snapshot.session_id(), snapshot.active_flow_id(), snapshot.active_node_id(),
               snapshot.created_at_unix_ms())
    , _started(true) {
    compileAllExpressions();
    initializeMetrics();
    for (const auto& [name, value] : snapshot.parameters()) {
        _session.setParameter(name, value);
    }
}

void OrchestratorEngine::initializeMetrics() {
    using namespace va::metrics;
    auto& collector = MetricsCollector::instance();
    _routeEvalLatencyMs.initialize(&collector,
        MetricDescriptor{"route_eval_latency_ms", MetricType::Sampler,
                         "Wall-clock time to evaluate + run one external turn's routes (spec §11)"});
    _routeMatchCount.initialize(&collector,
        MetricDescriptor{"route_match_count", MetricType::Counter,
                         "Number of routes whose executor actually ran (spec §11)"});
    _completionCount.initialize(&collector,
        MetricDescriptor{"completion_count", MetricType::Counter,
                         "Number of sessions that reached an END_SESSION transition (spec §11)"});
    _turnsToCompletion.initialize(&collector,
        MetricDescriptor{"turns_to_completion", MetricType::Sampler,
                         "Session turnCount at the moment END_SESSION applied (spec §11)"});
}

void OrchestratorEngine::compileAllExpressions() {
    auto compile = [this](const std::string& text) {
        if (text.empty() || _compiledExpressions.count(text)) return;
        _compiledExpressions.emplace(text, ExpressionEvaluator(text));
    };
    auto walkExecutor = [&](const Executor& executor) {
        for (const auto& action : executor.actions()) {
            if (action.kind_case() == Action::kSetParameter) {
                compile(action.set_parameter().value_cel());
            } else if (action.kind_case() == Action::kCustomTool) {
                for (const auto& [key, expr] : action.custom_tool().args_cel()) {
                    compile(expr);
                }
            }
            // call_webhook.args (Phase 3, M3.1) - not compiled here yet.
        }
    };
    for (const auto& flow : _agent.flows()) {
        for (const auto& node : flow.nodes()) {
            walkExecutor(node.entry_executor());
            for (const auto& route : node.routes()) {
                if (route.trigger().kind_case() == Trigger::kConditionCel) {
                    compile(route.trigger().condition_cel());
                }
                walkExecutor(route.executor());
            }
        }
    }
}

const ExpressionEvaluator& OrchestratorEngine::compiledExpression(const std::string& text) const {
    auto it = _compiledExpressions.find(text);
    if (it == _compiledExpressions.end()) {
        throw std::runtime_error("OrchestratorEngine: expression was not precompiled: '" + text + "'");
    }
    return it->second;
}

const Flow* OrchestratorEngine::findFlow(const std::string& flowId) const {
    for (const auto& flow : _agent.flows()) {
        if (flow.id() == flowId) return &flow;
    }
    return nullptr;
}

const Node* OrchestratorEngine::findNode(const Flow& flow, const std::string& nodeId) const {
    for (const auto& node : flow.nodes()) {
        if (node.id() == nodeId) return &node;
    }
    return nullptr;
}

const Node* OrchestratorEngine::currentNode() const {
    const Flow* flow = findFlow(_session.activeFlowId());
    if (!flow) return nullptr;
    return findNode(*flow, _session.activeNodeId());
}

std::vector<std::string> OrchestratorEngine::intentCandidateNames(const Node& node) const {
    std::vector<std::string> names;
    for (const auto& route : node.routes()) {
        if (route.trigger().kind_case() != Trigger::kIntent) continue;
        const std::string& name = route.trigger().intent().intent_name();
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

ExpressionEvaluator::Activation OrchestratorEngine::buildActivation(
    const google::protobuf::Struct* eventParams) const {
    ExpressionEvaluator::Activation activation;

    for (const auto& [name, value] : _session.parameters().fields()) {
        activation.parameters[name] = value;
    }

    google::protobuf::Value turnCountValue;
    turnCountValue.set_number_value(_session.turnCount());
    activation.parameters["turnCount"] = turnCountValue;

    // Not implemented yet - no reprompt loop until Phase 2's parameter
    // collection lands; always 0 until then.
    google::protobuf::Value repromptCountValue;
    repromptCountValue.set_number_value(0);
    activation.parameters["repromptCount"] = repromptCountValue;

    google::protobuf::Value elapsedValue;
    elapsedValue.set_number_value(
        static_cast<double>(nowUnixMs() - _session.createdAtUnixMs()));
    activation.parameters["sessionElapsedMs"] = elapsedValue;

    // Not implemented yet - the permission gate (spec §9.8) is Phase 4;
    // always empty until an app wires up real speaker identity.
    google::protobuf::Value speakerIdValue;
    speakerIdValue.set_string_value("");
    activation.parameters["speaker_id"] = speakerIdValue;

    if (eventParams) {
        for (const auto& [name, value] : eventParams->fields()) {
            activation.event[name] = value;
        }
    }

    // Milestone 2.3 (spec §6 step 2/§8) - this turn's cached NLUResult
    // (empty when no provider is attached, the active node has no intent
    // routes, or nothing matched), scoped for the whole external turn - see
    // the class comment on handleUtterance().
    for (const auto& [name, value] : _lastNluResult.extracted_parameters()) {
        activation.nlu[name] = value;
    }

    return activation;
}

std::string OrchestratorEngine::substituteParameters(const std::string& text) const {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            auto close = text.find('}', i + 1);
            if (close != std::string::npos) {
                std::string name = text.substr(i + 1, close - i - 1);
                auto value = _session.getParameter(name);
                if (value) {
                    out += valueToDisplayString(*value);
                    i = close + 1;
                    continue;
                }
                // No matching parameter - leave the placeholder text untouched
                // rather than silently substituting a blank (a typo'd
                // parameter name should be visible in the output, not hidden).
            }
        }
        out += text[i++];
    }
    return out;
}

void OrchestratorEngine::runAction(const Action& action, const google::protobuf::Struct* eventParams) {
    switch (action.kind_case()) {
    case Action::kSay: {
        std::string text = substituteParameters(action.say().text());
        if (_onSay) _onSay(text);
        break;
    }
    case Action::kSetParameter: {
        const auto& setParam = action.set_parameter();
        const auto& compiled = compiledExpression(setParam.value_cel());
        auto activation = buildActivation(eventParams); // rebuilt fresh so an
                                                          // earlier action's
                                                          // setParameter in the
                                                          // same executor is
                                                          // already visible.
        auto value = compiled.evaluate(activation);
        _session.setParameter(setParam.name(), value);
        break;
    }
    case Action::kClearParameter:
        _session.clearParameter(action.clear_parameter().name());
        break;
    case Action::kEmitEvent: {
        PendingEvent ev;
        ev.name = action.emit_event().name();
        ev.params = action.emit_event().params();
        _pendingEvents.push_back(std::move(ev));
        break;
    }
    case Action::kCustomTool: {
        // Spec §9.6 - an ordinary deterministic route's customTool call:
        // args_cel evaluated into a Struct (rebuilt fresh for the same reason
        // kSetParameter's activation is, above), handler invoked, return
        // value discarded. Only BLOCKING execution_mode is implemented - see
        // the class comment on why FIRE_AND_FORGET/BEST_EFFORT wait on M3.1's
        // shared thread pool.
        const auto& customTool = action.custom_tool();
        google::protobuf::Struct args;
        auto activation = buildActivation(eventParams);
        for (const auto& [key, expr] : customTool.args_cel()) {
            const auto& compiled = compiledExpression(expr);
            (*args.mutable_fields())[key] = compiled.evaluate(activation);
        }
        callCustomTool(customTool.tool(), args);
        break;
    }
    case Action::kLlmGenerate: {
        // Spec §9.3 - a one-shot, stateless call (own history cleared first,
        // same "keep it contamination-free" reasoning LLMNLUProvider's
        // detect() already applies to its own classification call): render
        // prompt_template like say.text ({param} substitution, not CEL - same
        // convention generative_config.system_prompt_template documents), send
        // it as the user turn, and speak whatever comes back verbatim - no
        // JSON contract here, unlike the generative-node loop below, since
        // this action's entire job is producing free-form text to say.
        if (!_llmTransport) {
            throw std::runtime_error("OrchestratorEngine: llmGenerate action requires an "
                                      "LLMTransport (see setLLMTransport)");
        }
        std::string prompt = substituteParameters(action.llm_generate().prompt_template());
        _llmTransport->clearHistory();
        auto frame = std::make_shared<va::transport::TextFrame>();
        frame->text = prompt;
        frame->timestamp = va::transport::Frame::getCurrentTimestamp();
        if (_llmTransport->sendFrame(frame)) {
            const auto& history = _llmTransport->getHistory();
            if (!history.empty() && history.back().role == va::llm::MessageRole::ASSISTANT && _onSay) {
                _onSay(history.back().content);
            }
        }
        break;
    }
    default:
        // call_webhook/handoff/expect - Phase 2/3 (spec §9.2, §9.5, §9.9).
        // Not yet implemented; ignored rather than guessed at.
        break;
    }
}

void OrchestratorEngine::runExecutor(const Executor& executor, const google::protobuf::Struct* eventParams) {
    for (const auto& action : executor.actions()) {
        runAction(action, eventParams);
    }
}

void OrchestratorEngine::enterNode(const std::string& flowId, const std::string& nodeId) {
    const Flow* flow = findFlow(flowId);
    if (!flow) {
        throw std::runtime_error("OrchestratorEngine: unknown flow id '" + flowId + "'");
    }
    const Node* node = findNode(*flow, nodeId);
    if (!node) {
        throw std::runtime_error("OrchestratorEngine: unknown node id '" + nodeId
                                  + "' in flow '" + flowId + "'");
    }

    _session.setActiveFlowId(flowId);
    _session.setActiveNodeId(nodeId);

    // Milestone 2.4 - fresh budget/history for whatever generative loop (if
    // any) this node runs; unread and harmless for a non-generative node.
    _generativeTurnsUsed = 0;

    runExecutor(node->entry_executor(), nullptr);

    PendingEvent nodeEnter;
    nodeEnter.name = "node_enter";
    _pendingEvents.push_back(std::move(nodeEnter));
}

void OrchestratorEngine::applyTransition(const Transition& transition) {
    bool preserveParameters = transition.has_preserve_parameters()
                                   ? transition.preserve_parameters().value()
                                   : true;

    switch (transition.target_type()) {
    case Transition::NODE: {
        if (!preserveParameters) _session.clearAllParameters();
        enterNode(_session.activeFlowId(), transition.target_id());
        break;
    }
    case Transition::FLOW: {
        const Flow* flow = findFlow(transition.target_id());
        if (!flow) {
            throw std::runtime_error("OrchestratorEngine: transition targets unknown flow id '"
                                      + transition.target_id() + "'");
        }
        if (!preserveParameters) _session.clearAllParameters();
        enterNode(flow->id(), flow->start_node_id());
        break;
    }
    case Transition::END_SESSION: {
        _ended = true;
        _completionCount.increment();
        _turnsToCompletion.observe(static_cast<double>(_session.turnCount()));
        if (_onFlowComplete) _onFlowComplete(_session.activeFlowId(), _session);
        break;
    }
    case Transition::NONE:
    default:
        break;
    }
}

google::protobuf::Struct OrchestratorEngine::callCustomTool(const std::string& toolName,
                                                            const google::protobuf::Struct& args) {
    auto it = _customToolHandlers.find(toolName);
    if (it == _customToolHandlers.end()) {
        throw std::runtime_error("OrchestratorEngine: no customTool handler registered for '"
                                  + toolName + "' (see setCustomToolHandler)");
    }
    return it->second(args, _session);
}

std::string OrchestratorEngine::buildGenerativeSystemPrompt(const GenerativeConfig& config) const {
    std::ostringstream ss;
    ss << substituteParameters(config.system_prompt_template());
    ss << "\n\nYou may call the following tools when appropriate. To call one, respond with "
          "ONLY a JSON object of the exact shape {\"tool\": \"<name>\", \"args\": {...}} and "
          "nothing else. Otherwise, just reply in plain text - that reply is spoken directly "
          "to the user.\n\nAvailable tools:\n";
    for (const auto& tool : config.tools()) {
        ss << "- " << tool.name();
        if (tool.terminal()) ss << " (calling this ends the interaction)";
        ss << "\n";
    }
    return ss.str();
}

void OrchestratorEngine::runGenerativeTurn(const Node& node, const std::string& utterance) {
    const GenerativeConfig& config = node.generative_config();
    const std::string exitEventName = config.exit_event().empty() ? "generative_exit" : config.exit_event();
    const int maxTurns = config.max_turns() > 0 ? config.max_turns() : _config.defaultGenerativeMaxTurns;

    // Spec §9.4 point 4 - "the loop ends... when maxTurns is reached,
    // whichever comes first": a turn budget already exhausted ends the loop
    // before this turn is even forwarded to the LLM.
    if (_generativeTurnsUsed >= maxTurns) {
        PendingEvent exitEvent;
        exitEvent.name = exitEventName;
        _pendingEvents.push_back(std::move(exitEvent));
        return;
    }

    if (!_llmTransport) {
        throw std::runtime_error("OrchestratorEngine: node '" + node.id()
                                  + "' is generative but no LLMTransport is attached (see setLLMTransport)");
    }

    ++_generativeTurnsUsed;
    if (_generativeTurnsUsed == 1) {
        // First turn since entering this node - fresh, contamination-free
        // history (same reasoning LLMNLUProvider's detect() already applies
        // to its own classification call), seeded with the templated system
        // prompt plus this engine's own tool-calling contract.
        _llmTransport->clearHistory();
        _llmTransport->addMessageToHistory(va::llm::MessageRole::SYSTEM, buildGenerativeSystemPrompt(config));
    }

    auto frame = std::make_shared<va::transport::TextFrame>();
    frame->text = utterance;
    frame->timestamp = va::transport::Frame::getCurrentTimestamp();
    if (!_llmTransport->sendFrame(frame)) {
        spdlog::warn("[OrchestratorEngine] generative node '{}': sendFrame() failed for \"{}\"",
                     node.id(), utterance);
        return;
    }

    // Not specified by spec §9.4, which assumes a well-behaved model: an
    // engine-only safety valve bounding how many back-to-back tool calls one
    // single external turn's exchange can make, so a model that never stops
    // calling tools can't hang this synchronous call forever.
    static constexpr int kMaxToolCallsPerTurn = 8;

    for (int i = 0; i < kMaxToolCallsPerTurn; ++i) {
        const auto& history = _llmTransport->getHistory();
        if (history.empty() || history.back().role != va::llm::MessageRole::ASSISTANT) {
            spdlog::warn("[OrchestratorEngine] generative node '{}': no assistant response", node.id());
            return;
        }
        const std::string raw = history.back().content;

        nlohmann::json parsed;
        bool isToolCall = false;
        try {
            parsed = nlohmann::json::parse(raw);
            isToolCall = parsed.is_object() && parsed.contains("tool") && parsed["tool"].is_string();
        } catch (const std::exception&) {
            isToolCall = false; // not JSON at all - the ordinary "plain text reply" case
        }

        if (!isToolCall) {
            if (_onSay) _onSay(raw);
            return;
        }

        const std::string toolName = parsed["tool"].get<std::string>();
        const GenerativeConfig::ToolRef* toolRef = findGenerativeToolRef(config, toolName);
        if (!toolRef) {
            // Spec §9.4 point 2 - "the LLM cannot see or call anything
            // outside that list", enforced here even if the model hallucinates
            // a name never listed in the prompt: refuse it as a tool-result
            // error and let the model recover, rather than dispatching to a
            // handler this node never opted into exposing.
            spdlog::warn("[OrchestratorEngine] generative node '{}': model called unavailable tool '{}'",
                         node.id(), toolName);
            _llmTransport->injectToolResult(toolName, "{\"error\":\"tool not available\"}");
            continue;
        }

        google::protobuf::Struct args;
        if (parsed.contains("args") && parsed["args"].is_object()) {
            args = jsonObjectToStruct(parsed["args"]);
        }
        google::protobuf::Struct result = callCustomTool(toolName, args);

        if (toolRef->terminal()) {
            // Spec §9.4 point 4 - ends "the instant" a terminal tool is
            // called; no further model turn is solicited.
            PendingEvent exitEvent;
            exitEvent.name = exitEventName;
            _pendingEvents.push_back(std::move(exitEvent));
            return;
        }

        _llmTransport->injectToolResult(toolName, structToJson(result).dump());
    }

    spdlog::warn("[OrchestratorEngine] generative node '{}': exceeded {} tool calls in one turn; "
                 "ending the turn without a reply", node.id(), kMaxToolCallsPerTurn);
}

bool OrchestratorEngine::fireRoute(const Route& route, const google::protobuf::Struct* eventParams) {
    _routeMatchCount.increment();
    runExecutor(route.executor(), eventParams);
    if (_ended) return true;

    bool transitioned = false;
    if (route.has_transition() && route.transition().target_type() != Transition::NONE) {
        applyTransition(route.transition());
        transitioned = true;
    }

    bool stopEvaluation = route.has_stop_evaluation() ? route.stop_evaluation().value() : true;
    return transitioned || stopEvaluation || _ended;
}

void OrchestratorEngine::evaluateRoutesOnce(const std::string* eventName,
                                            const google::protobuf::Struct* eventParams) {
    const Node* node = currentNode();
    if (!node) return;

    // Spec §6 step 3's ordering: event routes first, then intent routes
    // (Milestone 2.3), then condition routes, then (not implemented - see
    // class comment) the no-match fallback. Each group is evaluated in Route
    // priority order (lower first); ties keep declaration order.
    std::vector<const Route*> eventRoutes;
    std::vector<const Route*> intentRoutes;
    std::vector<const Route*> conditionRoutes;
    for (const auto& route : node->routes()) {
        switch (route.trigger().kind_case()) {
        case Trigger::kEvent:        eventRoutes.push_back(&route); break;
        case Trigger::kIntent:       intentRoutes.push_back(&route); break;
        case Trigger::kConditionCel: conditionRoutes.push_back(&route); break;
        default:                     break; // unset - never matches
        }
    }
    auto byPriority = [](const Route* a, const Route* b) { return a->priority() < b->priority(); };
    std::stable_sort(eventRoutes.begin(), eventRoutes.end(), byPriority);
    std::stable_sort(intentRoutes.begin(), intentRoutes.end(), byPriority);
    std::stable_sort(conditionRoutes.begin(), conditionRoutes.end(), byPriority);

    if (eventName) {
        for (const Route* route : eventRoutes) {
            if (route->trigger().event().name() != *eventName) continue;
            if (fireRoute(*route, eventParams)) return;
        }
    }

    // this turn's cached NLUResult (see handleUtterance) - empty top_intent
    // means "no match" (domain.proto's own comment on the field), so an
    // IntentTrigger can never spuriously match against it here.
    if (!_lastNluResult.top_intent().empty()) {
        for (const Route* route : intentRoutes) {
            const IntentTrigger& intentTrigger = route->trigger().intent();
            if (intentTrigger.intent_name() != _lastNluResult.top_intent()) continue;
            if (_lastNluResult.confidence() < intentTrigger.min_confidence()) continue;
            if (fireRoute(*route, eventParams)) return;
        }
    }

    for (const Route* route : conditionRoutes) {
        const auto& compiled = compiledExpression(route->trigger().condition_cel());
        auto activation = buildActivation(eventParams);
        auto result = compiled.evaluate(activation);
        if (result.kind_case() != google::protobuf::Value::kBoolValue) {
            throw std::runtime_error("OrchestratorEngine: route '" + route->id()
                                      + "' condition_cel must evaluate to a boolean");
        }
        if (!result.bool_value()) continue;
        if (fireRoute(*route, eventParams)) return;
    }
}

void OrchestratorEngine::runToQuiescence() {
    while (!_pendingEvents.empty() && !_ended) {
        PendingEvent ev = std::move(_pendingEvents.front());
        _pendingEvents.pop_front();
        evaluateRoutesOnce(&ev.name, &ev.params);
    }
}

void OrchestratorEngine::start() {
    if (_started) return;
    _started = true;

    va::metrics::ScopedTimer timer(_routeEvalLatencyMs.get());

    if (_agent.flows().empty()) {
        throw std::runtime_error("OrchestratorEngine: agent '" + _agent.id() + "' has no flows");
    }
    const Flow& flow = _agent.flows(0);
    enterNode(flow.id(), flow.start_node_id());
    runToQuiescence();
}

void OrchestratorEngine::handleUtterance(const std::string& text, const NLUResult* injectedResult) {
    if (!_started || _ended) return;

    va::metrics::ScopedTimer timer(_routeEvalLatencyMs.get());

    _session.incrementTurnCount();

    const Node* node = currentNode();
    if (node && node->generative()) {
        // Spec §9.4 - a generative node never touches NLUProvider and skips
        // §6 steps 1-3 entirely; runGenerativeTurn is the whole turn.
        runGenerativeTurn(*node, text);
        runToQuiescence();
        return;
    }

    if (injectedResult) {
        _lastNluResult = *injectedResult;
    } else {
        _lastNluResult = NLUResult{};
        if (_nluProvider && node) {
            auto candidates = intentCandidateNames(*node);
            // Skip the call entirely when the active node has no intent
            // routes at all - see the class comment on setNLUProvider/
            // handleUtterance for why this guard exists (no wasted
            // LLMNLUProvider round trip when nothing could consult the
            // result).
            if (!candidates.empty()) {
                NLUContext context;
                context.set_locale(_agent.default_locale());
                context.set_active_flow_id(_session.activeFlowId());
                context.set_active_node_id(_session.activeNodeId());
                for (const auto& name : candidates) {
                    context.add_candidate_intent_names(name);
                }
                _lastNluResult = _nluProvider->detect(text, context);
            }
        }
    }

    evaluateRoutesOnce(nullptr, nullptr);
    runToQuiescence();
}

void OrchestratorEngine::setNLUProvider(std::shared_ptr<NLUProvider> provider) {
    _nluProvider = std::move(provider);
}

void OrchestratorEngine::setLLMTransport(std::shared_ptr<va::llm::LLMTransport> transport) {
    _llmTransport = std::move(transport);
}

void OrchestratorEngine::setCustomToolHandler(
    std::string name, std::function<google::protobuf::Struct(const google::protobuf::Struct&, SessionState&)> handler) {
    _customToolHandlers[std::move(name)] = std::move(handler);
}

void OrchestratorEngine::setOnFlowComplete(std::function<void(const std::string&, const SessionState&)> cb) {
    _onFlowComplete = std::move(cb);
}

void OrchestratorEngine::setOnSay(std::function<void(const std::string&)> cb) {
    _onSay = std::move(cb);
}

const SessionState& OrchestratorEngine::sessionState() const { return _session; }
bool OrchestratorEngine::sessionEnded() const { return _ended; }

SessionSnapshot OrchestratorEngine::snapshot() const {
    SessionSnapshot snap;
    snap.set_session_id(_session.sessionId());
    snap.set_active_flow_id(_session.activeFlowId());
    snap.set_active_node_id(_session.activeNodeId());
    for (const auto& [name, value] : _session.parameters().fields()) {
        (*snap.mutable_parameters())[name] = value;
    }
    // parameter_meta/route_history/event_counters/pending_ask: nothing in
    // M1.6 tracks parameter source/attempts, route history, or expect state
    // (that's M1.7/Phase 2) - left empty rather than half-populated.
    snap.set_created_at_unix_ms(_session.createdAtUnixMs());
    snap.set_updated_at_unix_ms(nowUnixMs());
    return snap;
}

} // namespace orchestrator
} // namespace va
