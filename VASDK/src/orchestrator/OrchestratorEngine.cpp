#include <VASDK/orchestrator/OrchestratorEngine.h>

#include <VASDK/Frame.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <thread>
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

// Milestone 3.1 (spec §9.2) - "one retry on transient/network errors only,
// never on 4xx": CurlHttpProvider sets NetworkErrorCode::HttpError with
// httpStatusCode populated for any non-2xx/3xx response (see
// curlCodeToError in CurlHttpProvider.cpp), so a 4xx reads as HttpError with
// httpStatusCode < 500 here - permanent, no retry. Every other NetworkError
// (Timeout, ConnectionFailed, TlsError, SendFailed, ReceiveFailed,
// LibraryError, ProtocolError) and a 5xx HttpError are transient.
bool isTransientFailure(const va::network::HttpResponse& resp) {
    if (!resp.error) return false;
    if (resp.error.code == va::network::NetworkErrorCode::HttpError) {
        return resp.error.httpStatusCode >= 500;
    }
    return true;
}

// Spec §18 leaves the exact circuit-breaker shape open ("failure-count
// threshold opens it for a cooldown window", no numbers given) - these are
// an engine-chosen default, the same kind of spec-silent constant
// kMaxToolCallsPerTurn already is for the generative loop (M2.4).
constexpr int kCircuitBreakerFailureThreshold = 3;
constexpr std::chrono::seconds kCircuitBreakerCooldown{30};

// requestflow-backend-spec.md §2 - the fixed request shape every
// callWebhook action sends; CallWebhookAction itself carries no args/body
// field (see domain.proto), so this is the same for every call, never
// author-customized.
nlohmann::json buildWebhookRequestJson(const std::string& sessionId, int turnIndex, const std::string& traceId,
                                       const std::string& flowId, const std::string& nodeId,
                                       const std::string& routeId, const std::string& utterance,
                                       const google::protobuf::Struct& parameters) {
    nlohmann::json body;
    body["contractVersion"] = "1.0";
    body["sessionId"] = sessionId;
    body["turnIndex"] = turnIndex;
    body["traceId"] = traceId;
    body["activeFlowId"] = flowId;
    body["activeNodeId"] = nodeId;
    body["routeId"] = routeId;
    if (!utterance.empty()) body["utterance"] = utterance;
    body["parameters"] = structToJson(parameters);
    return body;
}

} // namespace

// Milestone 3.1 (spec §9.1/§9.2) - the off-thread pool callWebhook actions
// run on, sized OrchestratorConfig::executorThreadPoolSize. Defined here in
// va::orchestrator directly (NOT the anonymous namespace above) so it's the
// same type OrchestratorEngine.h forward-declares for its pimpl'd unique_ptr
// member - an anonymous-namespace definition would be a different,
// unrelated type of the same name. submit() is only ever called from this
// translation unit, so nothing needs it exposed in the header itself.
class WebhookThreadPool {
public:
    explicit WebhookThreadPool(size_t numThreads) {
        for (size_t i = 0; i < std::max<size_t>(1, numThreads); ++i) {
            _workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~WebhookThreadPool() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stop = true;
        }
        _cv.notify_all();
        for (auto& worker : _workers) {
            if (worker.joinable()) worker.join();
        }
    }

    std::future<va::network::HttpResponse> submit(std::function<va::network::HttpResponse()> task) {
        auto packaged = std::make_shared<std::packaged_task<va::network::HttpResponse()>>(std::move(task));
        std::future<va::network::HttpResponse> future = packaged->get_future();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _tasks.push([packaged] { (*packaged)(); });
        }
        _cv.notify_one();
        return future;
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait(lock, [this] { return _stop || !_tasks.empty(); });
                if (_stop && _tasks.empty()) return;
                task = std::move(_tasks.front());
                _tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> _workers;
    std::queue<std::function<void()>> _tasks;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _stop = false;
};

OrchestratorEngine::OrchestratorEngine(va::orchestrator::Agent agent, OrchestratorConfig config)
    : _agent(std::move(agent))
    , _config(config)
    , _session(generateSessionId(), "", "") {
    _webhookThreadPool = std::make_unique<WebhookThreadPool>(_config.executorThreadPoolSize);
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
    _webhookThreadPool = std::make_unique<WebhookThreadPool>(_config.executorThreadPoolSize);
    compileAllExpressions();
    initializeMetrics();
    for (const auto& [name, value] : snapshot.parameters()) {
        _session.setParameter(name, value);
    }
    // Milestone 2.5 (spec §9.5) - a mid-ask session must survive restore.
    for (const auto& [name, meta] : snapshot.parameter_meta()) {
        _session.setParameterMeta(name, meta);
    }
    if (snapshot.has_pending_ask()) {
        _session.setPendingAsk(snapshot.pending_ask());
    }
}

// Milestone 3.1 - must be defined here (not implicitly in the header), and
// only after WebhookThreadPool's own definition above - see the declaration
// in OrchestratorEngine.h.
OrchestratorEngine::~OrchestratorEngine() = default;

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
    _parameterValidationFailures.initialize(&collector,
        MetricDescriptor{"parameter_validation_failures", MetricType::Counter,
                         "Number of failed static-form/expect parameter validation attempts (spec §11)"});
    _webhookLatencyMs.initialize(&collector,
        MetricDescriptor{"webhook_latency_ms", MetricType::Sampler,
                         "Wall-clock time a callWebhook action waited on its result, including its one "
                         "retry if any (spec §11)"});
    _webhookErrorCount.initialize(&collector,
        MetricDescriptor{"webhook_error_count", MetricType::Counter,
                         "Number of callWebhook actions that ended in the webhook_error fallback (spec §11)"});
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
            // CallWebhookAction has no CEL-expression field of its own (see
            // domain.proto) - its request body is entirely engine-constructed
            // per requestflow-backend-spec.md §2, never author-templated -
            // so there's nothing for callWebhook to precompile here.
        }
    };
    // Milestone 3.1 - EventHandlers.webhook_error is the first EventHandlers
    // executor this engine ever actually runs (handleWebhookFailure); a CEL
    // expression inside one (e.g. a setParameter reading event.status_code)
    // needs precompiling here exactly like a route's executor does, or
    // compiledExpression() throws "not precompiled" the first time it runs.
    // Walking every EventHandlers field, not just webhook_error, costs
    // nothing now and avoids the same gap for no_match/no_input/user_cancel/
    // permission_denied/webhook_deferred once those are implemented.
    auto walkHandlers = [&](const EventHandlers& handlers) {
        walkExecutor(handlers.no_match());
        walkExecutor(handlers.no_input());
        walkExecutor(handlers.webhook_error());
        walkExecutor(handlers.user_cancel());
        walkExecutor(handlers.permission_denied());
        walkExecutor(handlers.webhook_deferred());
    };
    walkHandlers(_agent.global_handlers());
    for (const auto& flow : _agent.flows()) {
        walkHandlers(flow.flow_handlers());
        for (const auto& node : flow.nodes()) {
            walkExecutor(node.entry_executor());
            walkHandlers(node.node_handlers());
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

    // Milestone 2.5 (spec §8/§9.5) - only a PendingAsk's attempts count here:
    // a static-form parameter's own reprompt cycle never reaches step 3 at
    // all (a failed extraction attempt ends the turn immediately - see
    // handleParameterExtractionTurn), so repromptCount only ever needs to be
    // nonzero for whatever route condition an `expect` answer's fall-through
    // to step 3 might consult, and even then it's already back to 0 by the
    // time that fall-through runs (the PendingAsk cleared on success, before
    // this activation is built for route evaluation).
    google::protobuf::Value repromptCountValue;
    repromptCountValue.set_number_value(_session.hasPendingAsk() ? _session.pendingAsk().attempts() : 0);
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

void OrchestratorEngine::runAction(const Action& action, const google::protobuf::Struct* eventParams,
                                   const std::string& routeId) {
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
    case Action::kExpect: {
        // Spec §9.5 - says parameter.prompts.initial (typed) exactly like a
        // SayAction, arms a PendingAsk, and ends the turn right here: nothing
        // after `expect` in this Executor's actions runs (validate() already
        // rejects a config where it isn't last; _suspendTurn is the runtime
        // backstop for an unvalidated one, e.g. this milestone's own
        // self-test configs).
        const auto& expect = action.expect();
        PendingAsk ask;
        if (expect.has_parameter()) {
            const Parameter& param = expect.parameter();
            if (!param.prompts().initial().empty() && _onSay) {
                _onSay(substituteParameters(param.prompts().initial()));
            }
            *ask.mutable_parameter() = param;
            ask.set_transient(expect.transient());
        }
        // Bare turn-wait (expect.has_parameter() == false): PendingAsk.parameter
        // stays absent, transient is meaningless per domain.proto's own comment,
        // nothing is said - spec §9.5's "Bare turn-wait" is silent by design.
        ask.set_attempts(0);
        _session.setPendingAsk(std::move(ask));
        _suspendTurn = true;
        break;
    }
    case Action::kCallWebhook:
        // Spec §9.2 - see handleWebhookAction()/class comment for the full
        // timeout/retry/circuit-breaker/fallback mechanics.
        handleWebhookAction(action.call_webhook(), routeId);
        break;
    default:
        // handoff - not yet implemented (M3.3, "handoff/escalation
        // end-to-end"); ignored rather than guessed at, same as
        // call_webhook was before this milestone.
        break;
    }
}

void OrchestratorEngine::runExecutor(const Executor& executor, const google::protobuf::Struct* eventParams,
                                     const std::string& routeId) {
    for (const auto& action : executor.actions()) {
        runAction(action, eventParams, routeId);
        // Milestone 2.5 - an `expect` action ends the turn; defense-in-depth
        // against an unvalidated config where it isn't already the last
        // action (OrchestratorConfigLoader::validate() rejects that case).
        // Milestone 3.1 - _ended is now also reachable mid-executor (a
        // webhook response's own `transition`/`endSession`, applied via
        // applyTransition() from inside handleWebhookAction/
        // applyWebhookResponse, nested inside this same runAction() call);
        // _abortExecutor stops the rest of a *failing* callWebhook action's
        // executor the same way (see handleWebhookFailure).
        if (_ended || _suspendTurn || _abortExecutor) break;
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

    runExecutor(node->entry_executor(), nullptr, "");
    // Milestone 2.5 - an `expect` in entry_executor already ended the turn
    // (see runExecutor); no node_enter cascade to queue on top of that.
    // Milestone 3.1 - a failed callWebhook in entry_executor already ran its
    // webhook_error fallback (handleWebhookFailure); _abortExecutor is
    // consumed-and-reset right here, same as fireRoute does below.
    if (_ended || _suspendTurn || _abortExecutor) {
        _abortExecutor = false;
        return;
    }

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

const Executor* OrchestratorEngine::findWebhookErrorHandler() const {
    const Node* node = currentNode();
    if (node && node->node_handlers().has_webhook_error()) {
        return &node->node_handlers().webhook_error();
    }
    const Flow* flow = findFlow(_session.activeFlowId());
    if (flow && flow->flow_handlers().has_webhook_error()) {
        return &flow->flow_handlers().webhook_error();
    }
    if (_agent.global_handlers().has_webhook_error()) {
        return &_agent.global_handlers().webhook_error();
    }
    return nullptr;
}

void OrchestratorEngine::handleWebhookFailure(const std::string& url, int statusCode,
                                              const std::string& errorMessage) {
    _webhookErrorCount.increment();

    google::protobuf::Struct params;
    google::protobuf::Value statusCodeValue;
    statusCodeValue.set_number_value(statusCode);
    (*params.mutable_fields())["status_code"] = statusCodeValue;
    google::protobuf::Value errorMessageValue;
    errorMessageValue.set_string_value(errorMessage);
    (*params.mutable_fields())["error_message"] = errorMessageValue;
    google::protobuf::Value urlValue;
    urlValue.set_string_value(url);
    (*params.mutable_fields())["url"] = urlValue;

    // Spec §9.2 - "re-enter route evaluation at step 3... so
    // EventHandlers.webhook_error (node, then flow, then agent) decides what
    // happens next": this handler chain *is* the fallback mechanism itself
    // (domain.proto's own comment on EventHandlers - deliberately not
    // something a Route.trigger.event could ever match), not an ordinary
    // emitted event. Running its Executor here, synchronously, stands in for
    // step 4 ("execute the matched executor's actions") on this re-entered
    // pass; EventHandlers has no Transition of its own, so there's no step 5
    // to apply here - an author reaches one only indirectly, via an
    // emitEvent a real node Route can catch, drained by the same
    // runToQuiescence() this whole external call already returns into (no
    // recursive draining added here).
    const Executor* handler = findWebhookErrorHandler();
    if (handler) {
        runExecutor(*handler, &params, "");
    }
    _abortExecutor = true;
}

void OrchestratorEngine::applyWebhookResponse(const std::string& rawJsonBody) {
    // requestflow-backend-spec.md §3 - "an empty {} is a valid, no-op
    // response"; extended here to "unparseable is also a no-op" rather than
    // treating a malformed body as a harder failure spec never describes.
    nlohmann::json parsed;
    try {
        parsed = rawJsonBody.empty() ? nlohmann::json::object() : nlohmann::json::parse(rawJsonBody);
    } catch (const std::exception& e) {
        spdlog::warn("[OrchestratorEngine] webhook response was not valid JSON: {} (raw: \"{}\")",
                     e.what(), rawJsonBody);
        return;
    }
    if (!parsed.is_object()) return;

    // "messages... before anything else in the response is applied."
    if (parsed.contains("messages") && parsed["messages"].is_array()) {
        for (const auto& message : parsed["messages"]) {
            if (message.is_string() && _onSay) _onSay(message.get<std::string>());
        }
    }
    if (parsed.contains("parameterPatches") && parsed["parameterPatches"].is_object()) {
        for (const auto& [key, value] : parsed["parameterPatches"].items()) {
            google::protobuf::Value protoValue;
            jsonValueToProtoValue(value, protoValue);
            // "webhook_patch" - ParameterMeta.source's own proto comment
            // already names this exact string.
            _session.setParameter(key, protoValue, "webhook_patch");
        }
    }
    if (parsed.contains("emitEvents") && parsed["emitEvents"].is_array()) {
        for (const auto& eventName : parsed["emitEvents"]) {
            if (!eventName.is_string()) continue;
            // backend-spec §3 - "a webhook response cannot attach event.*
            // params the way a customTool handler can" - name only.
            PendingEvent ev;
            ev.name = eventName.get<std::string>();
            _pendingEvents.push_back(std::move(ev));
        }
    }
    if (parsed.contains("transition") && parsed["transition"].is_object()) {
        const auto& t = parsed["transition"];
        std::string targetType = t.value("targetType", "");
        Transition transition;
        if (targetType == "NODE") {
            transition.set_target_type(Transition::NODE);
        } else if (targetType == "FLOW") {
            transition.set_target_type(Transition::FLOW);
        } else if (targetType == "END_SESSION") {
            transition.set_target_type(Transition::END_SESSION);
        } else {
            transition.set_target_type(Transition::NONE);
        }
        transition.set_target_id(t.value("targetId", ""));
        if (transition.target_type() != Transition::NONE) applyTransition(transition);
    }
    if (parsed.value("endSession", false) && !_ended) {
        // Reuses applyTransition's own END_SESSION case (completion metrics,
        // onFlowComplete) rather than duplicating it here.
        Transition endTransition;
        endTransition.set_target_type(Transition::END_SESSION);
        applyTransition(endTransition);
    }
    // `handoff` (backend-spec §3's `{reason}` object) - deliberately not
    // implemented: "fires setOnHandoff the same as a HandoffAction" needs
    // setOnHandoff/HandoffAction wiring this engine doesn't have yet (M3.3,
    // "handoff/escalation end-to-end") - ignored the same "not yet
    // specified, not guessed at" way HandoffAction's own runAction case
    // still is.
}

void OrchestratorEngine::handleWebhookAction(const CallWebhookAction& webhook, const std::string& routeId) {
    if (!_httpInterface) {
        throw std::runtime_error("OrchestratorEngine: callWebhook action requires an "
                                  "HttpInterface (see setHttpInterface)");
    }

    const std::string url = webhook.url();
    const std::string method = webhook.method().empty() ? "POST" : webhook.method();
    const int perCallTimeoutMs =
        webhook.timeout_ms() > 0 ? webhook.timeout_ms() : static_cast<int>(_config.webhookTimeout.count());

    // Spec §9.2 - a small circuit breaker per endpoint. See the class
    // comment for why kCircuitBreakerFailureThreshold/kCircuitBreakerCooldown
    // are engine-chosen constants rather than something spec gives a number
    // for.
    auto& circuit = _webhookCircuits[url];
    auto now = std::chrono::steady_clock::now();
    if (circuit.consecutiveFailures >= kCircuitBreakerFailureThreshold && now < circuit.openUntil) {
        throw std::runtime_error("OrchestratorEngine: circuit breaker open for webhook endpoint '"
                                  + url + "' (spec §9.2) - an unrecoverable failure, the same "
                                  "'fail loudly' treatment this class already gives a missing "
                                  "LLMTransport/customTool handler/HttpInterface");
    }

    std::string requestBody = buildWebhookRequestJson(_session.sessionId(), _session.turnCount(),
                                                       _session.sessionId() + ":" + std::to_string(_session.turnCount()),
                                                       _session.activeFlowId(), _session.activeNodeId(), routeId,
                                                       _lastUtterance, _session.parameters())
                                   .dump();

    auto attemptOnce = [this, url, method, requestBody, perCallTimeoutMs]() -> va::network::HttpResponse {
        std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
        if (method == "GET") return _httpInterface->get(url, headers, perCallTimeoutMs);
        return _httpInterface->post(url, headers, requestBody, perCallTimeoutMs);
    };

    // Spec §9.2 - "one retry on transient/network errors only, never on
    // 4xx" - the whole attempt(+retry) sequence is one submitted task, so
    // waiting on its single future below bounds both attempts together by
    // turnExternalCallBudget, not just the first.
    const int maxRetries = _config.webhookMaxRetries;
    std::future<va::network::HttpResponse> future = _webhookThreadPool->submit([attemptOnce, maxRetries]() {
        va::network::HttpResponse response = attemptOnce();
        int retriesLeft = maxRetries;
        while (retriesLeft > 0 && isTransientFailure(response)) {
            response = attemptOnce();
            --retriesLeft;
        }
        return response;
    });

    va::metrics::ScopedTimer webhookTimer(_webhookLatencyMs.get());
    std::future_status status = future.wait_for(_config.turnExternalCallBudget);

    if (status != std::future_status::ready) {
        // Outer turnExternalCallBudget exceeded - distinct from a per-call
        // timeout_ms expiry (already covered by isTransientFailure/retry
        // above). The task keeps running to completion on the pool thread;
        // its result is simply discarded - the same "abandon the in-flight
        // future" precedent §9.3 already establishes for a barge-in
        // interrupt, applied here to a budget expiry instead.
        circuit.consecutiveFailures++;
        if (circuit.consecutiveFailures >= kCircuitBreakerFailureThreshold) {
            circuit.openUntil = std::chrono::steady_clock::now() + kCircuitBreakerCooldown;
        }
        handleWebhookFailure(url, 0, "turnExternalCallBudget exceeded");
        return;
    }

    va::network::HttpResponse response = future.get();
    if (!response.error) {
        circuit.consecutiveFailures = 0;
        applyWebhookResponse(response.body);
        return;
    }

    circuit.consecutiveFailures++;
    if (circuit.consecutiveFailures >= kCircuitBreakerFailureThreshold) {
        circuit.openUntil = std::chrono::steady_clock::now() + kCircuitBreakerCooldown;
    }
    handleWebhookFailure(url, response.statusCode, response.error.message);
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
    runExecutor(route.executor(), eventParams, route.id());
    // Milestone 2.5 - an `expect` in this route's executor already ended the
    // turn; its own Transition (if any) never applies on top of that, same as
    // an END_SESSION reached mid-executor already short-circuits here.
    // Milestone 3.1 - a failed callWebhook already ran the webhook_error
    // fallback chain (handleWebhookFailure) and decided what happens next
    // itself; this route's own Transition is skipped the same way, and
    // _abortExecutor is consumed-and-reset right here (see class comment -
    // unlike _suspendTurn, it never needs to propagate past this point).
    if (_ended || _suspendTurn || _abortExecutor) {
        _abortExecutor = false;
        return true;
    }

    bool transitioned = false;
    if (route.has_transition() && route.transition().target_type() != Transition::NONE) {
        applyTransition(route.transition());
        transitioned = true;
    }

    bool stopEvaluation = route.has_stop_evaluation() ? route.stop_evaluation().value() : true;
    return transitioned || stopEvaluation || _ended || _suspendTurn;
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
    while (!_pendingEvents.empty() && !_ended && !_suspendTurn) {
        PendingEvent ev = std::move(_pendingEvents.front());
        _pendingEvents.pop_front();
        evaluateRoutesOnce(&ev.name, &ev.params);
    }
}

void OrchestratorEngine::finishTurn() {
    runToQuiescence();
    if (!_ended && !_suspendTurn) checkFormParameterAsk();
}

const Parameter* OrchestratorEngine::findUnfilledFormParameter(const Node& node) const {
    auto activation = buildActivation(nullptr);
    for (const auto& p : node.form()) {
        if (_session.getParameter(p.name()).has_value()) continue;
        if (ParameterValidator::isRequired(p, activation)) return &p;
    }
    return nullptr;
}

void OrchestratorEngine::checkFormParameterAsk() {
    if (_session.hasPendingAsk()) return; // structurally shouldn't happen - see class comment
    const Node* node = currentNode();
    if (!node || node->generative()) return;

    const Parameter* param = findUnfilledFormParameter(*node);
    if (!param) return;

    _session.noteParameterAskStarted(param->name());
    if (!param->prompts().initial().empty() && _onSay) {
        _onSay(substituteParameters(param->prompts().initial()));
    }
}

void OrchestratorEngine::handleParameterExtractionTurn(const Parameter& parameter, const std::string& text,
                                                        const NLUResult* injectedResult) {
    NLUResult result;
    if (injectedResult) {
        result = *injectedResult;
    } else if (_nluProvider) {
        NLUContext context;
        context.set_locale(_agent.default_locale());
        context.set_active_flow_id(_session.activeFlowId());
        context.set_active_node_id(_session.activeNodeId());
        context.set_expected_parameter_name(parameter.name());
        result = _nluProvider->detect(text, context);
    }
    _lastNluResult = result;

    auto it = result.extracted_parameters().find(parameter.name());
    bool hasValue = it != result.extracted_parameters().end();
    ParameterValidationResult validation;
    if (hasValue) {
        const google::protobuf::Value& extracted = it->second;
        if (parameter.type() == Parameter::OBJECT && extracted.kind_case() == google::protobuf::Value::kStructValue) {
            validation = _parameterValidator.validateObject(parameter, extracted);
        } else {
            validation = _parameterValidator.validateAndNormalize(parameter, valueToDisplayString(extracted));
        }
    }

    bool isPendingAsk = _session.hasPendingAsk();

    if (hasValue && validation.valid) {
        // Captured before any mutation below: for a typed PendingAsk,
        // `parameter` aliases _session.pendingAsk().parameter() - clearPendingAsk()
        // (called just below) clears that storage out from under the reference.
        const std::string paramName = parameter.name();

        std::string source = isPendingAsk ? "expect" : "nlu";
        _session.setParameter(paramName, validation.normalizedValue, source);

        bool transient = isPendingAsk && _session.pendingAsk().transient();
        if (isPendingAsk) _session.clearPendingAsk();

        // Spec §9.5 - "evaluation falls through to step 3 the same turn."
        evaluateRoutesOnce(nullptr, nullptr);
        // "the value is removed from parameters again immediately after the
        // route that consumes it finishes evaluating" - before any
        // node_enter/emitEvent cascade finishTurn() drains next.
        if (transient) _session.clearParameter(paramName);
        finishTurn();
        return;
    }

    // Spec §6 step 2 - "select the next reprompt (this step does not consult
    // routes)": nothing extracted, or it didn't validate. Both cases share one
    // reprompt/escalation cycle; the validator's own errorMessage (when
    // present) is diagnostic only - see the class comment on why it isn't
    // what's spoken.
    if (hasValue && !validation.valid) {
        spdlog::debug("[OrchestratorEngine] parameter '{}' failed validation: {}",
                      parameter.name(), validation.errorMessage);
    }
    _parameterValidationFailures.increment();

    int attempts = isPendingAsk ? _session.incrementPendingAskAttempts()
                                 : _session.incrementParameterAttempts(parameter.name());

    const auto& reprompts = parameter.prompts().reprompts();
    std::string message;
    if (attempts <= reprompts.size()) {
        message = reprompts[attempts - 1];
    } else {
        message = parameter.prompts().escalation();
    }
    if (!message.empty() && _onSay) {
        _onSay(substituteParameters(message));
    }
}

void OrchestratorEngine::start() {
    if (_started) return;
    _started = true;
    _suspendTurn = false;
    _abortExecutor = false;

    va::metrics::ScopedTimer timer(_routeEvalLatencyMs.get());

    if (_agent.flows().empty()) {
        throw std::runtime_error("OrchestratorEngine: agent '" + _agent.id() + "' has no flows");
    }
    const Flow& flow = _agent.flows(0);
    enterNode(flow.id(), flow.start_node_id());
    finishTurn();
}

void OrchestratorEngine::handleUtterance(const std::string& text, const NLUResult* injectedResult) {
    if (!_started || _ended) return;

    va::metrics::ScopedTimer timer(_routeEvalLatencyMs.get());

    _session.incrementTurnCount();
    _suspendTurn = false;
    _abortExecutor = false;
    // Milestone 3.1 - requestflow-backend-spec.md §2's `utterance` request
    // field for any callWebhook action reached during this external turn.
    _lastUtterance = text;

    const Node* node = currentNode();
    if (node && node->generative()) {
        // Spec §9.4 - a generative node never touches NLUProvider and skips
        // §6 steps 1-3 entirely; runGenerativeTurn is the whole turn.
        runGenerativeTurn(*node, text);
        finishTurn();
        return;
    }

    // Milestone 2.5 (spec §6 step 2/§9.5) - the priority chain: a PendingAsk
    // (an ExpectAction armed on a prior turn) first, then the active node's
    // static form, then (falling through below) ordinary intent candidates.
    const Parameter* expectedParameter = nullptr;
    if (_session.hasPendingAsk()) {
        const PendingAsk& ask = _session.pendingAsk();
        if (ask.has_parameter()) {
            expectedParameter = &ask.parameter();
        } else {
            // Bare turn-wait (spec §9.5) - nothing to extract; clear it and
            // fall straight through to whatever a fresh turn on this node
            // would do (form check, then ordinary classification), per spec's
            // own "exactly as if the node had just started a fresh turn."
            _session.clearPendingAsk();
        }
    }
    if (!expectedParameter && node) {
        expectedParameter = findUnfilledFormParameter(*node);
    }

    if (expectedParameter) {
        handleParameterExtractionTurn(*expectedParameter, text, injectedResult);
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
    finishTurn();
}

void OrchestratorEngine::setNLUProvider(std::shared_ptr<NLUProvider> provider) {
    _nluProvider = std::move(provider);
}

void OrchestratorEngine::registerParameterValidator(const std::string& name, ParameterValidator::CustomValidator fn) {
    _parameterValidator.registerValidator(name, std::move(fn));
}

void OrchestratorEngine::setLLMTransport(std::shared_ptr<va::llm::LLMTransport> transport) {
    _llmTransport = std::move(transport);
}

void OrchestratorEngine::setHttpInterface(std::shared_ptr<va::network::HttpInterface> http) {
    _httpInterface = std::move(http);
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
    // Milestone 2.5 (spec §9.5) - route_history/event_counters still travel
    // empty (nothing in this milestone's scope needs them).
    for (const auto& [name, meta] : _session.parameterMetaAll()) {
        (*snap.mutable_parameter_meta())[name] = meta;
    }
    if (_session.hasPendingAsk()) {
        *snap.mutable_pending_ask() = _session.pendingAsk();
    }
    snap.set_created_at_unix_ms(_session.createdAtUnixMs());
    snap.set_updated_at_unix_ms(nowUnixMs());
    return snap;
}

} // namespace orchestrator
} // namespace va
