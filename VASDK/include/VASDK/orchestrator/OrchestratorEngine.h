#pragma once

#include "vasdk_export.h"
#include <VASDK/Metrics/MetricHandles.h>
#include <VASDK/orchestrator/ExpressionEvaluator.h>
#include <VASDK/orchestrator/SessionState.h>
#include <orchestrator/domain.pb.h>

#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace va {
namespace orchestrator {

// Plain C++ struct - in-process engine tuning, never serialized, so it stays
// outside the proto schema. Spec §13, quoted in full even though M1.6 only
// consumes defaultNoInputTimeout/defaultMaxReprompts indirectly (neither is
// active yet - no reprompt loop exists until parameter collection ships in
// Phase 2): this is the one config struct the whole engine takes, not a
// milestone-scoped subset of it, so a field with no consumer yet is exactly
// as inert as Parameter.no_input_timeout_ms already sitting unused in the
// M1.1 proto.
struct VASDK_EXPORT OrchestratorConfig {
    std::chrono::milliseconds webhookTimeout{1500};
    int webhookMaxRetries = 1;
    std::chrono::milliseconds turnExternalCallBudget{2000};
    std::chrono::milliseconds defaultNoInputTimeout{5000};
    size_t executorThreadPoolSize = 2;
    int defaultMaxReprompts = 2;
    int defaultGenerativeMaxTurns = 6;
};

// Milestone 1.6 (docs/reference/requestflow-roadmap.md §3) - implements spec
// §6 steps 3-6 for `event` and `condition` routes only:
//
//   - No `intent` routes: NLUProvider is Phase 2 (M2.1-M2.3). An event route
//     only ever matches a named internally-emitted event (node_enter or
//     EmitEventAction); a condition route is evaluated every time step 3 runs,
//     regardless of what triggered it, since condition_cel isn't gated by an
//     event name at all.
//   - Only four Action kinds run: say, setParameter, clearParameter,
//     emitEvent. callWebhook/customTool/llmGenerate/handoff/expect are
//     Phase 2/3 (§9.2-§9.6, §9.9) - encountering one is a silent no-op for
//     now rather than a guess at behavior no milestone has specified yet.
//   - No permission gate (§9.8, Phase 4), no generative nodes (§9.4, M2.4).
//     Session persistence itself (§10) landed in M1.7 as a pair: snapshot()
//     below, plus the restoring constructor and SessionPersistence/
//     FileSessionPersistence (VASDK/include/VASDK/orchestrator/
//     SessionPersistence.h) - parameter_meta/route_history/event_counters/
//     pending_ask still aren't tracked (nothing populates them before
//     Phase 2's expect/webhook/NLU machinery exists), so they still travel
//     empty through both snapshot() and restore.
//
// Event timing, a genuine judgment call the spec text underdetermines: a
// node's entryExecutor is followed by an auto-emitted `node_enter` that
// re-enters step 3 "immediately... with no utterance in between" (spec §6's
// last paragraph) - so node_enter cascades happen synchronously, inside the
// same call, not deferred to wait for a fresh external stimulus. An ordinary
// EmitEventAction is treated identically (same pending-event queue, drained
// the same way) rather than as a special one-turn-delayed case: nothing in
// the spec gives emitEvent a different cadence from node_enter, and treating
// it as deferred would make an event-driven graph unable to act on its own
// output without waiting for the user to speak again, which the `triage`
// worked example (spec §19) doesn't do either.
//
// An internally-emitted event that matches no route on the active node is a
// silent no-op, not EventHandlers.no_match: no_match is specifically "no
// intent was recognized" (spec §6's step 3 fallback chain reads naturally as
// the outcome of an NLU classification attempt), and most nodes will have no
// event route at all for events they don't care about - that's the ordinary
// steady state (waiting for the next input), not an error.
//
// Milestone 1.8 (docs/reference/requestflow-roadmap.md §3) added spec §11's
// per-turn metrics into the existing MetricsCollector, via
// MetricsCollector::instance() (same default-singleton pattern as
// va::transport::BaseTransport - no injection setter, since spec §13 lists
// no setMetricsCollector on OrchestratorEngine). Only four of §11's metrics
// have a real producer in M1.6/M1.7's scope, so only these four are wired:
// route_eval_latency_ms (Sampler, one observation per external turn -
// start() or handleUtterance() - covering that turn's whole route-eval-and-
// cascade work), route_match_count (Counter, once per route that actually
// fires), completion_count (Counter, once per END_SESSION transition), and
// turns_to_completion (Sampler, session turnCount at that same moment).
// no_match_count/no_input_count/parameter_validation_failures need Phase 2's
// NLU/expect machinery; nlu_*/webhook_*/handoff_count/fire_and_forget_*/
// best_effort_* need Phase 2/3/4 features this engine doesn't implement yet -
// wiring them now would mean a counter that can never increment, which is
// worse than not registering it at all.
class VASDK_EXPORT OrchestratorEngine {
public:
    explicit OrchestratorEngine(va::orchestrator::Agent agent, OrchestratorConfig config = {});

    // Milestone 1.7 (session persistence, spec §10) - restores a session
    // previously captured by snapshot() and round-tripped through a
    // SessionPersistence implementation. The same Agent config the session
    // started with must be supplied: the snapshot carries only runtime
    // values (parameters, active flow/node, timestamps), never routes/nodes,
    // per spec §10's "the app must supply the definition" rule (same
    // precedent as goal/'s FileGoalStatePersistence + GoalDefinition split).
    //
    // Does not replay the active node's entry_executor or the node_enter
    // cascade - the node was already entered (and its executor already ran)
    // before the snapshot was taken, so replaying it here would re-fire
    // say/setParameter/emitEvent side effects a resumed session never asked
    // for. The session is marked started immediately (a second start() call
    // afterward is the existing idempotent no-op). turnCount is not part of
    // SessionSnapshot (spec §4) and so is not restored - it resets to 0,
    // consistent with nothing in M1.7's done-criterion (parameters/
    // active_node_id fidelity) depending on it.
    OrchestratorEngine(va::orchestrator::Agent agent, const va::orchestrator::SessionSnapshot& snapshot,
                        OrchestratorConfig config = {});

    // Begins the session: enters agent.flows(0)'s start_node_id (runs its
    // entry_executor, then the node_enter cascade). Idempotent - a second
    // call is a no-op. "Which flow starts a fresh session" has no dedicated
    // field in the M1.1 schema (only repeated Flow), so this follows spec
    // §9.7's own precedent that flow declaration *order* is meaningful and
    // takes flows(0) as the entry flow.
    void start();

    // Feeds one finalized utterance into route evaluation. M1.6 has no
    // NLUProvider, so nothing is extracted or classified from `text` yet
    // (Phase 2); only condition routes on the active node are (re-)evaluated,
    // since an event route can never match without a named event.
    void handleUtterance(const std::string& text);

    // Spec §13 - fires once, the instant a Transition resolves to
    // END_SESSION (this milestone's reading of "flow complete").
    void setOnFlowComplete(std::function<void(const std::string& flowId, const SessionState&)> cb);

    // Every `say` action's already-{parameter}-substituted text, in firing
    // order. OrchestratorFrameProcessor is the real consumer (turns this into
    // downstream TextFrames); exposed as a callback here so the engine itself
    // has no Frame/FrameProcessor dependency.
    void setOnSay(std::function<void(const std::string& text)> cb);

    const SessionState& sessionState() const;
    bool sessionEnded() const;

    // Read-only projection onto the persisted proto shape (spec §4/§10) -
    // what FileSessionPersistence::save() writes. The restoring constructor
    // above is the other half of this round trip.
    va::orchestrator::SessionSnapshot snapshot() const;

private:
    struct PendingEvent {
        std::string name;
        google::protobuf::Struct params;
    };

    va::orchestrator::Agent _agent;
    OrchestratorConfig _config;
    SessionState _session;
    bool _started = false;
    bool _ended = false;

    std::deque<PendingEvent> _pendingEvents;
    std::unordered_map<std::string, ExpressionEvaluator> _compiledExpressions;

    std::function<void(const std::string&, const SessionState&)> _onFlowComplete;
    std::function<void(const std::string&)> _onSay;

    // Milestone 1.8 (spec §11) - see the class comment above for exactly
    // which four metrics these are and why the rest aren't registered yet.
    va::metrics::SamplerHandle _routeEvalLatencyMs;
    va::metrics::CounterHandle _routeMatchCount;
    va::metrics::CounterHandle _completionCount;
    va::metrics::SamplerHandle _turnsToCompletion;
    void initializeMetrics();

    void compileAllExpressions();
    const ExpressionEvaluator& compiledExpression(const std::string& text) const;

    const va::orchestrator::Flow* findFlow(const std::string& flowId) const;
    const va::orchestrator::Node* findNode(const va::orchestrator::Flow& flow, const std::string& nodeId) const;
    const va::orchestrator::Node* currentNode() const;

    ExpressionEvaluator::Activation buildActivation(const google::protobuf::Struct* eventParams) const;
    std::string substituteParameters(const std::string& text) const;

    void enterNode(const std::string& flowId, const std::string& nodeId);
    void runExecutor(const va::orchestrator::Executor& executor, const google::protobuf::Struct* eventParams);
    void runAction(const va::orchestrator::Action& action, const google::protobuf::Struct* eventParams);
    void applyTransition(const va::orchestrator::Transition& transition);

    // Returns true when this evaluation pass should stop (a transition fired,
    // the session ended, or the matched route's stop_evaluation - default
    // true - says so).
    bool fireRoute(const va::orchestrator::Route& route, const google::protobuf::Struct* eventParams);
    void evaluateRoutesOnce(const std::string* eventName, const google::protobuf::Struct* eventParams);
    void runToQuiescence();
};

} // namespace orchestrator
} // namespace va
