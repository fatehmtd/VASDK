#pragma once

#include "vasdk_export.h"
#include <VASDK/Metrics/MetricHandles.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/orchestrator/ExpressionEvaluator.h>
#include <VASDK/orchestrator/NLUProvider.h>
#include <VASDK/orchestrator/SessionState.h>
#include <orchestrator/domain.pb.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

// Milestone 1.6 (docs/reference/requestflow-roadmap.md §3) implemented spec
// §6 steps 3-6 for `event` and `condition` routes only; Milestone 2.3 (§4)
// added `intent` routes on top, using whichever NLUProvider (M2.1/M2.2) is
// attached via setNLUProvider() - or none at all, which is still a fully
// supported shape (spec §2's "LLM-optional by construction": an agent with
// no intent routes anywhere never needs a provider, and one is never called
// for a node that has no intent-triggered routes of its own).
//
//   - `NLUProvider.detect()` (spec §6 step 2) runs once per external
//     handleUtterance() call, only when the active node has at least one
//     `intent`-triggered route (candidate_intent_names gathered from exactly
//     those routes' IntentTrigger.intent_name, deduped, declaration order) -
//     calling a provider (an LLM API call, for LLMNLUProvider) when nothing
//     on the node could possibly consult the result would be pure waste.
//     expected_parameter_name is never set: that's step 2's "required
//     parameter missing" branch, which needs the `expect`/form machinery
//     M2.5 hasn't shipped yet, so every detect() call here is the "no
//     pending parameter" branch. context.interrupt_intent_names (§6.2) is
//     always left empty and NLUResult.co_intents (§6.1) is never consulted -
//     both are Phase 4 items per the roadmap's own table, same deferral
//     TrainingPhraseNLUProvider/LLMNLUProvider already made.
//   - The turn's NLUResult (real or, for OrchestratorConversationSimulator,
//     injected - spec §12) is cached for the rest of that external turn,
//     including any node_enter/emitEvent cascade runToQuiescence() drains
//     afterward: an `intent` route matches when its IntentTrigger.intent_name
//     equals that cached result's top_intent and top_intent's confidence
//     meets IntentTrigger.min_confidence, evaluated in step 3 right after
//     event routes and before condition routes (spec §6's ordering), on
//     every evaluateRoutesOnce() pass exactly like condition routes already
//     are - no special-casing to restrict it to only the first pass, which
//     would be new machinery none of spec §6's text asks for. Its
//     extracted_parameters populate the nlu.* namespace ExpressionEvaluator
//     already reserved for this back in M1.4, visible to condition_cel/
//     value_cel/args_cel for the rest of the turn.
//   - Event routes only ever match a named internally-emitted event
//     (node_enter or EmitEventAction); a condition route is evaluated every
//     time step 3 runs, regardless of what triggered it, since condition_cel
//     isn't gated by an event name at all.
//   - Milestone 2.3 added say/setParameter/clearParameter/emitEvent plus
//     intent-route matching. Milestone 2.4 (this milestone) added customTool
//     (§9.6, both as an ordinary route action and as the mechanism a
//     generative node's LLM-tool loop dispatches through) and llmGenerate
//     (§9.3, a one-shot templated-prompt call whose raw reply becomes this
//     turn's say output) plus generative nodes themselves (§9.4). callWebhook/
//     handoff/expect remain Phase 2/3 (§9.2, §9.5, §9.9) - encountering one is
//     a silent no-op for now rather than a guess at behavior no milestone has
//     specified yet.
//   - Generative nodes (§9.4): while the active node has generative: true,
//     handleUtterance() skips NLUProvider.detect() and §6 steps 1-3 entirely
//     (a generative node never touches NLUProvider) and instead runs
//     runGenerativeTurn(), which drives _llmTransport directly - a system
//     message (generative_config.system_prompt_template, {param}-substituted
//     like say.text, plus a tool-calling contract this engine defines: reply
//     with only {"tool": "<name>", "args": {...}} to call one of
//     generative_config.tools, anything else is treated as plain spoken text)
//     installed once per node-entry, then one user-turn's worth of
//     tool-call/tool-result exchanges (each dispatched through the same
//     setCustomToolHandler registry an ordinary route's CustomToolAction
//     uses - spec §9.4 point 3's "the ordinary customTool handler") until the
//     model produces plain text (this turn's say output) or calls a
//     `terminal` tool, whichever ends the loop first and emits
//     generative_config.exit_event (default "generative_exit") as a pending
//     event, re-entering ordinary route evaluation at step 3 exactly like any
//     other emitted event. _generativeTurnsUsed - reset whenever any node is
//     entered - bounds how many *user* turns the loop runs before it force-
//     exits the same way (spec's "whichever comes first"); a separate,
//     engine-only kMaxToolCallsPerTurn (OrchestratorEngine.cpp) bounds how
//     many back-to-back tool calls one single turn's exchange can make,
//     since nothing in spec §9.4 bounds that and an unbounded loop would hang
//     this synchronous call forever against a misbehaving model.
//   - Not implemented: the off-thread thread pool/turnExternalCallBudget and
//     ControlFrame::INTERRUPT barge-in cancellation §9.1/§9.3 describe for
//     every external call (llmGenerate, the generative loop's LLM calls,
//     customTool's FIRE_AND_FORGET/BEST_EFFORT modes) - this engine still has
//     no threading infrastructure at all (OrchestratorConfig::
//     executorThreadPoolSize stays an inert tuning field, same as M1.6 left
//     it), so every external call here runs synchronously in BLOCKING mode
//     only, same scope decision M2.2's LLMNLUProvider already made for its
//     own classification round trip. That infrastructure is shared with
//     M3.1's callWebhook reliability model - the natural place to build it
//     once, for every off-thread action kind at once, rather than
//     duplicating a one-off version here.
//   - A customTool handler or an attempted llmGenerate/generative-node call
//     against a name/transport the app never wired up throws
//     std::runtime_error, the same "fail loudly" treatment enterNode already
//     gives an unknown flow/node id - both are the same class of bug (config
//     assumes wiring the app never completed), not a runtime condition worth
//     degrading gracefully around.
//   - No permission gate (§9.8, Phase 4).
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

    // Feeds one finalized utterance into route evaluation (spec §6 step 2-3).
    // `injectedResult`, when non-null, is used as this turn's NLUResult
    // verbatim instead of calling the attached provider (or skipping
    // classification if none is attached) - this is exactly what
    // OrchestratorConversationSimulator::setInjectedNLUResult (spec §12)
    // needs for deterministic, model-free tests of intent routes, and it
    // applies regardless of whether the active node actually has any intent
    // routes, unlike the real-provider path below.
    void handleUtterance(const std::string& text, const va::orchestrator::NLUResult* injectedResult = nullptr);

    // Attaches the NLUProvider `intent` routes are matched against (spec §6
    // step 2/13). Unset (the default) is a fully supported shape: intent
    // routes simply never match, identical to M1.6's behavior. Ownership is
    // shared, not exclusive - the same provider instance can back more than
    // one engine (e.g. LLMNLUProvider's own class comment already assumes a
    // dedicated-but-possibly-reused transport).
    void setNLUProvider(std::shared_ptr<va::orchestrator::NLUProvider> provider);

    // Milestone 2.4 (spec §9.3/§9.4/§13) - attaches the transport llmGenerate
    // actions and generative nodes call. Unset is a fully supported shape as
    // long as the agent never actually reaches a node/action that needs it -
    // same "optional until a feature needs it" pattern setNLUProvider already
    // established; unlike that one, though, an agent that *does* declare a
    // generative node or llmGenerate action with no transport attached throws
    // at the point it's actually reached (see class comment), since there's
    // no graceful degradation for "the node whose entire purpose is this
    // call has nothing to call."
    void setLLMTransport(std::shared_ptr<va::llm::LLMTransport> transport);

    // Milestone 2.4 (spec §9.6) - registers the deterministic C++ handler
    // behind a customTool action's `tool` name. Invoked identically whether
    // the call came from an ordinary route's CustomToolAction (args_cel
    // evaluated into a Struct, return value discarded - spec §9.6) or a
    // generative node's LLM-tool loop (the model's JSON tool-call args
    // converted to a Struct, the handler's returned Struct converted back to
    // JSON as that tool's result - spec §9.4 point 3). A route or a
    // generative node's tools list naming an unregistered tool throws (see
    // class comment) rather than silently doing nothing.
    void setCustomToolHandler(
        std::string name,
        std::function<google::protobuf::Struct(const google::protobuf::Struct&, SessionState&)> handler);

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

    // Milestone 2.3 (spec §6 step 2/3) - unset provider is a valid, fully
    // supported configuration (see setNLUProvider above); _lastNluResult is
    // this turn's cached classification (real or injected), read by both
    // evaluateRoutesOnce (intent-route matching) and buildActivation
    // (nlu.* namespace) for the rest of the external turn it was produced in.
    std::shared_ptr<va::orchestrator::NLUProvider> _nluProvider;
    va::orchestrator::NLUResult _lastNluResult;

    // Milestone 2.4 (spec §9.3/§9.4/§9.6) - see the class comment above.
    // _generativeTurnsUsed counts user turns consumed by the *current* node's
    // generative loop; reset to 0 on every enterNode() call (harmless, unread
    // for a non-generative node).
    std::shared_ptr<va::llm::LLMTransport> _llmTransport;
    std::unordered_map<std::string,
                       std::function<google::protobuf::Struct(const google::protobuf::Struct&, SessionState&)>>
        _customToolHandlers;
    int _generativeTurnsUsed = 0;

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

    // Deduped, declaration-order IntentTrigger.intent_name values across
    // `node`'s intent-triggered routes - spec §6 step 2's "candidateIntentNames
    // = node routes".
    std::vector<std::string> intentCandidateNames(const va::orchestrator::Node& node) const;

    ExpressionEvaluator::Activation buildActivation(const google::protobuf::Struct* eventParams) const;
    std::string substituteParameters(const std::string& text) const;

    void enterNode(const std::string& flowId, const std::string& nodeId);
    void runExecutor(const va::orchestrator::Executor& executor, const google::protobuf::Struct* eventParams);
    void runAction(const va::orchestrator::Action& action, const google::protobuf::Struct* eventParams);
    void applyTransition(const va::orchestrator::Transition& transition);

    // Milestone 2.4 (spec §9.6) - looks up and invokes a registered
    // customTool handler; throws if `toolName` was never registered (see
    // class comment).
    google::protobuf::Struct callCustomTool(const std::string& toolName, const google::protobuf::Struct& args);

    // Milestone 2.4 (spec §9.4) - drives one external turn of a generative
    // node's LLM-tool loop against `node`. See the class comment above for
    // the full mechanics.
    void runGenerativeTurn(const va::orchestrator::Node& node, const std::string& utterance);
    std::string buildGenerativeSystemPrompt(const va::orchestrator::GenerativeConfig& config) const;

    // Returns true when this evaluation pass should stop (a transition fired,
    // the session ended, or the matched route's stop_evaluation - default
    // true - says so).
    bool fireRoute(const va::orchestrator::Route& route, const google::protobuf::Struct* eventParams);
    void evaluateRoutesOnce(const std::string* eventName, const google::protobuf::Struct* eventParams);
    void runToQuiescence();
};

} // namespace orchestrator
} // namespace va
