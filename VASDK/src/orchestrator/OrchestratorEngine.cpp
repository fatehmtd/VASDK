#include <VASDK/orchestrator/OrchestratorEngine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

} // namespace

OrchestratorEngine::OrchestratorEngine(va::orchestrator::Agent agent, OrchestratorConfig config)
    : _agent(std::move(agent))
    , _config(config)
    , _session(generateSessionId(), "", "") {
    compileAllExpressions();
}

OrchestratorEngine::OrchestratorEngine(va::orchestrator::Agent agent, const SessionSnapshot& snapshot,
                                       OrchestratorConfig config)
    : _agent(std::move(agent))
    , _config(config)
    , _session(snapshot.session_id(), snapshot.active_flow_id(), snapshot.active_node_id(),
               snapshot.created_at_unix_ms())
    , _started(true) {
    compileAllExpressions();
    for (const auto& [name, value] : snapshot.parameters()) {
        _session.setParameter(name, value);
    }
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
            }
            // custom_tool.args_cel / call_webhook - Phase 2/3, not compiled here yet.
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
    default:
        // call_webhook/handoff/custom_tool/llm_generate/expect - Phase 2/3
        // (spec §9.2-§9.6, §9.9). Not yet implemented; ignored rather than
        // guessed at.
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
        if (_onFlowComplete) _onFlowComplete(_session.activeFlowId(), _session);
        break;
    }
    case Transition::NONE:
    default:
        break;
    }
}

bool OrchestratorEngine::fireRoute(const Route& route, const google::protobuf::Struct* eventParams) {
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

    // Spec §6 step 3's ordering: event routes first, then (Phase 2) intent
    // routes, then condition routes, then (not implemented - see class
    // comment) the no-match fallback. Each group is evaluated in Route
    // priority order (lower first); ties keep declaration order.
    std::vector<const Route*> eventRoutes;
    std::vector<const Route*> conditionRoutes;
    for (const auto& route : node->routes()) {
        switch (route.trigger().kind_case()) {
        case Trigger::kEvent:        eventRoutes.push_back(&route); break;
        case Trigger::kConditionCel: conditionRoutes.push_back(&route); break;
        default:                     break; // intent / unset - never matches in M1.6
        }
    }
    auto byPriority = [](const Route* a, const Route* b) { return a->priority() < b->priority(); };
    std::stable_sort(eventRoutes.begin(), eventRoutes.end(), byPriority);
    std::stable_sort(conditionRoutes.begin(), conditionRoutes.end(), byPriority);

    if (eventName) {
        for (const Route* route : eventRoutes) {
            if (route->trigger().event().name() != *eventName) continue;
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

    if (_agent.flows().empty()) {
        throw std::runtime_error("OrchestratorEngine: agent '" + _agent.id() + "' has no flows");
    }
    const Flow& flow = _agent.flows(0);
    enterNode(flow.id(), flow.start_node_id());
    runToQuiescence();
}

void OrchestratorEngine::handleUtterance(const std::string& /*text*/) {
    if (!_started || _ended) return;
    _session.incrementTurnCount();
    // M1.6 has no NLUProvider (Phase 2), so nothing is extracted or
    // classified from the utterance yet - only condition routes (never
    // gated by an event name) are eligible on a plain re-entry like this.
    evaluateRoutesOnce(nullptr, nullptr);
    runToQuiescence();
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
