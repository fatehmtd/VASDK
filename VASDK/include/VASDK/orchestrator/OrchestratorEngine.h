#pragma once

#include "vasdk_export.h"
#include <VASDK/Metrics/MetricHandles.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/network/HttpInterface.h>
#include <VASDK/orchestrator/ExpressionEvaluator.h>
#include <VASDK/orchestrator/NLUProvider.h>
#include <VASDK/orchestrator/ParameterValidator.h>
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

// Milestone 3.1 (spec §9.2) - the off-thread pool callWebhook actions run on.
// Forward-declared/pimpl'd here (full definition lives in OrchestratorEngine.cpp,
// the only translation unit that ever calls submit()) so this header doesn't
// need <future>/<thread> just to declare the unique_ptr member below.
class WebhookThreadPool;

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
//     on the node could possibly consult the result would be pure waste -
//     except the call is no longer skippable purely on that ground: spec
//     §6.2's interrupt-intents milestone widened the guard to "candidates OR
//     mergedInterruptIntentNames(*node) is non-empty", since a node with no
//     intent routes of its own can still have an agent/flow/node-scoped
//     interrupt set worth checking. context.interrupt_intent_names is now
//     always populated with that merged set on this call (see
//     mergedInterruptIntentNames()). NLUResult.co_intents (spec 6.1, multi-
//     intent co-firing) is consulted too, but only inside evaluateRoutesOnce()
//     - see that method's own comment for why it's a separate per-co_intent
//     loop rather than something threaded through detect()'s guard here.
//   - Milestone 2.5 (spec §9.5/§7) - `expected_parameter_name` IS now set,
//     per turn, following the exact priority chain spec §6 step 2 describes:
//     (1) a PendingAsk (`_session.hasPendingAsk()`) - set by a prior turn's
//     ExpectAction - wins if its `parameter` is present (a typed expect);
//     (2) otherwise the active node's `form`, first required-and-unfilled
//     Parameter in declaration order (findUnfilledFormParameter(), reusing
//     ParameterValidator::isRequired() for the condition_cel check); (3)
//     otherwise the ordinary candidate_intent_names path above. A bare
//     PendingAsk (no `parameter`) short-circuits straight back to (2)/(3) -
//     spec §9.5's "exactly as if the node had just started a fresh turn" -
//     after being cleared, since there's nothing to extract for it.
//     handleParameterExtractionTurn() does the rest: detect() (or
//     `injectedResult`, same convention as the ordinary path) runs with
//     `expected_parameter_name` set; a value found in
//     NLUResult.extracted_parameters is validated via `_parameterValidator`
//     (ParameterValidator::validateObject() when the Parameter is OBJECT-typed
//     *and* the extracted Value is already struct-shaped - skipping a raw-
//     string JSON round trip through the one ParameterValidator::
//     validateAndNormalize() branch that still uses
//     google::protobuf::util::JsonStringToMessage internally, see that file -
//     validateAndNormalize() via valueToDisplayString() otherwise). On
//     success: `_session.setParameter(name, normalized, source)` (source
//     "expect" for a PendingAsk, "nlu" for a static-form parameter - spec
//     §9.5's own `source: "expect"` tagging), the PendingAsk clears if that's
//     what was answered (its `transient` value is removed from parameters
//     again immediately after this turn's step-3 route evaluation, before any
//     node_enter/emitEvent cascade - spec §9.5's own wording), and evaluation
//     falls through to step 3 the same turn. On failure (nothing extracted,
//     or it didn't validate): "this step does not consult routes" (spec §6
//     step 2) - the engine picks `prompts.reprompts[attempts-1]` or
//     `prompts.escalation` once attempts exceeds the reprompt list (a
//     PendingAsk's attempts live on PendingAsk.attempts itself, since that
//     value has to travel with the snapshot; a form parameter's live in
//     SessionState's own ParameterMeta, keyed by name) and the turn ends right
//     there - no route evaluation, no cascade. The validator's own
//     errorMessage is logged (spdlog::debug), not spoken - spec §7 leaves the
//     exact relationship between a validation error and the spoken reprompt
//     underspecified, and this engine's own authored `prompts.reprompts` text
//     is what a voice UI should actually say, the same judgment call
//     `goal/`'s GoalManager already made logging (not speaking)
//     SlotValidationResult::errorMessage.
//   - Milestone 2.5 also added the `expect` action itself (runAction's
//     Action::kExpect case): says `parameter.prompts.initial` (typed) or
//     nothing (bare) exactly like a SayAction, arms a PendingAsk via
//     `_session.setPendingAsk()`, and sets `_suspendTurn = true` - a new
//     per-external-turn flag (reset at the top of start()/handleUtterance())
//     that ends this call's processing immediately: runExecutor() stops
//     running further actions in the same Executor (defense-in-depth -
//     OrchestratorConfigLoader::validate() already rejects a config where
//     `expect` isn't an Executor's last action), fireRoute() treats it like
//     `_ended` (skips applying that route's own Transition, if any - an
//     `expect` action ending the turn takes priority), enterNode() skips
//     queuing `node_enter` when entry_executor itself suspended the turn, and
//     runToQuiescence()'s cascade loop stops. checkFormParameterAsk() (called
//     from the new finishTurn() helper, itself replacing every bare
//     runToQuiescence() call site) is what actually says a static-form
//     parameter's `prompts.initial` the first time it's noticed - the moment
//     a node freshly settles (session start, or after any transition/cascade)
//     with a required, unfilled, not-yet-asked form parameter (spec §19's
//     collect_reason/collect_date worked example) - and is skipped whenever
//     the turn already ended some other way (_ended or _suspendTurn).
//   - The `no-input` half of spec §9.5 (an upstream silence timeout ending an
//     ask via EventHandlers.no_input, `Parameter.no_input_timeout_ms`/
//     `ControlFrame::SET_LISTEN_TIMEOUT`) is NOT implemented: there is still
//     no OrchestratorFrameProcessor-side turn-detection integration to signal
//     it from (the M1.6-era OrchestratorFrameProcessor only ever calls
//     handleUtterance() with real recognized text), so this milestone only
//     implements the validation-failure reprompt/escalation cycle, which is
//     fully exercisable through OrchestratorConversationSimulator today.
//   - Spec §6.2 (interrupt intents / mid-task cancel, resolving the Draft
//     v2.1 §18 open item on EventHandlers.user_cancel having no trigger
//     path): mergedInterruptIntentNames(node) - Agent.interrupt_intent_names
//     ∪ active Flow's ∪ node's - is populated into
//     NLUContext.interrupt_intent_names on every detect() call this class
//     makes: handleParameterExtractionTurn()'s slot-filling call (both the
//     static-form and pending-ask case), the ordinary classification call
//     above (whose "skip when nothing could consult it" guard now also
//     checks this merged set, not just candidate_intent_names), and a new
//     interrupt-only prelude call runGenerativeTurn() makes ahead of a
//     generative node's LLM turn (see its own comment). Both v1 NLUProvider
//     implementations check this set first, regardless of what else a given
//     call is for (see their own class comments) - so when the returned
//     NLUResult.top_intent is itself a member, handleParameterExtractionTurn()
//     skips extraction/storage entirely (clearing a PendingAsk first, if one
//     was active, so it can't go stale across whatever transition the
//     matched route applies) and falls straight through to an ordinary step-3
//     evaluateRoutesOnce() pass with that intent as the winner - "just a
//     normal route like any other" per spec, so an ordinary node-level
//     intent-triggered Route catches it exactly like any other intent match.
//     If nothing does (evaluateRoutesOnce()'s existing anyRouteFired tracking,
//     M3.3, stays false), evaluateRoutesOnce() calls handleUserCancel()
//     instead of handleNoMatch() for that one pass - the merged set is
//     re-derived there too, to tell "no route caught the matched interrupt"
//     apart from "nothing matched at all" (the latter still routes to
//     no_match, unchanged). This is what finally gives
//     EventHandlers.user_cancel a trigger path.
//   - Spec §6.1 (multi-intent co-firing) inserts a second loop into
//     evaluateRoutesOnce() right after top_intent's own intentRoutes group
//     and before conditionRoutes - spec §6 step 3's own ordering ("then
//     intent routes (including any co_intents...), then condition routes"):
//     one independent route search per entry in NLUResult.co_intents
//     (populated by the provider, empty by default - Draft v2.1's own
//     behavior when it's unpopulated), against this node's
//     Route.co_fireable-flagged intent routes only, in co_intents order.
//     This loop always runs, even when top_intent's own group already said
//     to stop looking further (its default stop_evaluation - previously
//     that meant "return from evaluateRoutesOnce() immediately", which would
//     have skipped this loop entirely; now that early return is deferred via
//     a local stopAfterTopIntentGroup flag, checked again only once this
//     loop is done, so condition routes/no-match still get skipped exactly
//     when they always did, but co_intents can never be silently skipped by
//     it). Each co_intent's own search is deliberately independent of the
//     others' stop_evaluation outcome too - see the loop's own comment for
//     why - so co_fireable routes on unrelated intents all get a chance to
//     fire in the same turn, the entire point of the feature ("play some
//     jazz and turn up the AC" firing both routes off one utterance rather
//     than forcing two turns). OrchestratorConfigLoader::validate() rejects,
//     at load time, any node where more than one intent route that could
//     co-fire together (at least one of them Route.co_fireable) also carries
//     a Transition - spec's "at most one...may carry a non-empty Transition"
//     conflict rule is a static config error, not something this loop has to
//     detect at runtime.
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
//     intent-route matching. Milestone 2.4 added customTool (§9.6, both as an
//     ordinary route action and as the mechanism a generative node's LLM-tool
//     loop dispatches through) and llmGenerate (§9.3, a one-shot
//     templated-prompt call whose raw reply becomes this turn's say output)
//     plus generative nodes themselves (§9.4). Milestone 2.5 added `expect`
//     (§9.5, both typed and bare - see below) and static-form parameter
//     collection (§7). Milestone 3.1 added callWebhook (§9.2) - see below.
//     Milestone 3.3 (this milestone) added `handoff`/escalation event
//     handlers end-to-end - see below.
//   - Milestone 3.1 (spec §9.2) - callWebhook (runAction's Action::kCallWebhook
//     case, handleWebhookAction()) is the first action kind that actually runs
//     off the calling thread: a WebhookThreadPool (sized
//     OrchestratorConfig::executorThreadPoolSize, owned via a pimpl'd
//     unique_ptr so this header stays free of <future>/<thread>) runs the
//     blocking HttpInterface::post()/get() call - and, per spec, its one
//     permitted retry - as a single submitted task, while the calling thread
//     waits on the returned std::future bounded by
//     OrchestratorConfig::turnExternalCallBudget (shared across whatever
//     external calls happen in one turn - simplified here to apply in full to
//     each individual callWebhook action, since nothing in spec §9.2
//     describes sub-dividing it across multiple calls in the same turn, and
//     multiple callWebhook actions in one Executor is an edge case, not the
//     documented scenario). A per-endpoint circuit breaker
//     (kCircuitBreakerFailureThreshold consecutive failures opens it for
//     kCircuitBreakerCooldown - both engine-chosen constants, since spec §18
//     leaves the exact retry/backoff/circuit-breaker shape open beyond "one
//     retry on transient errors") refuses a call outright once open, throwing
//     std::runtime_error - spec's own "unrecoverable failures... surface as
//     ErrorFrame upstream," the same "fail loudly" treatment this class
//     already gives a missing LLMTransport/customTool handler/HttpInterface,
//     since OrchestratorFrameProcessor has no ErrorFrame-wrapping of its own
//     yet to route it through (a pre-existing gap, not new to this
//     milestone). isTransientFailure() (OrchestratorEngine.cpp anonymous
//     namespace) is what spec's "never on 4xx" rule is built on: a 4xx
//     HttpResponse::error (NetworkErrorCode::HttpError with
//     httpStatusCode < 500) is permanent and never retried; a 5xx, or any
//     other NetworkError (timeout, connection failure, TLS, ...), is
//     transient and gets the one retry OrchestratorConfig::webhookMaxRetries
//     allows.
//   - On success, applyWebhookResponse() parses the JSON body per
//     requestflow-backend-spec.md §3 (`messages`, `parameterPatches`,
//     `transition`, `emitEvents`, `handoff`, `endSession`) and applies each
//     field in that order, reusing
//     existing machinery throughout: `messages` become `_onSay()` calls
//     verbatim (no {param} substitution - this is backend-authored text, not
//     flow config), `parameterPatches` become `_session.setParameter(name,
//     value, "webhook_patch")` (that literal source string is already named
//     in ParameterMeta's own proto comment), `emitEvents` become bare-named
//     PendingEvents (backend-spec §3: a webhook response cannot attach
//     event.* params the way a customTool handler can), and `transition`/
//     `endSession` both go through the existing applyTransition() (endSession
//     builds a synthetic Transition{END_SESSION} rather than duplicating
//     applyTransition's own completion-metrics/onFlowComplete logic). An
//     empty or malformed JSON body is a no-op (backend-spec §3's own "empty
//     {} is a valid no-op response" rule, extended to "can't parse it either"
//     rather than treating a malformed response as a harder failure spec
//     never describes).
//   - On failure - retries exhausted, or the outer turnExternalCallBudget
//     wait timed out (the task itself is left running to completion on the
//     pool and its result discarded, the same "abandon the in-flight future"
//     precedent §9.3 already establishes for a barge-in interrupt, just
//     applied to a budget expiry instead) - handleWebhookFailure() runs
//     whatever Executor findWebhookErrorHandler() resolves via the node ->
//     flow -> agent EventHandlers.webhook_error chain (domain.proto's own
//     comment on EventHandlers: this is a distinct fallback mechanism, not
//     something an ordinary Route.trigger.event could ever match), with
//     `event.status_code`/`error_message`/`url` populated per spec's own
//     table (§8.2). This *is* spec's "re-enter route evaluation at step 3" -
//     EventHandlers.webhook_error's Executor stands in for "the matched
//     executor" that step would otherwise run; it has no Transition of its
//     own (EventHandlers isn't Route-shaped), so an author reaches one from
//     here only indirectly, by emitEvent-ing something a real node Route can
//     catch - drained by the same runToQuiescence() this whole external call
//     already returns into, no recursive draining added. A new per-call
//     `_abortExecutor` flag (reset at the top of every start()/
//     handleUtterance(), exactly like `_suspendTurn`, though unlike
//     `_suspendTurn` it never needs to propagate past the immediately
//     enclosing runExecutor()/fireRoute()/enterNode() - both consume and
//     reset it the instant they see it) is what stops the *failing* route's
//     own remaining actions and skips its own Transition, without ending the
//     turn or suspending it the way `_ended`/`_suspendTurn` do - the session
//     stays live, the fallback chain decided what happens next instead.
//   - setHttpInterface() attaches the va::network::HttpInterface callWebhook
//     dispatches through - unset is not a valid configuration for an agent
//     that actually reaches a callWebhook action (throws, same "wiring never
//     completed" class of error as a missing LLMTransport); a fake
//     HttpInterface is exactly what OrchestratorConversationSimulator/
//     self-tests inject for deterministic, no-live-endpoint webhook_error/
//     retry/circuit-breaker coverage (spec §16's own acceptance criterion).
//   - routeId now threads through runExecutor()/runAction() (previously just
//     `eventParams`) - the one new piece of per-call context callWebhook's
//     request body needs (requestflow-backend-spec.md §2's `routeId` field)
//     that nothing before this milestone required. Empty for entry_executor
//     calls (no "matched route" exists yet) and for the webhook_error
//     handler's own executor (same reasoning).
//   - Milestone 3.3 (spec §13/§20.1, roadmap "handoff/escalation event
//     handlers end-to-end") adds two things, both synchronous - neither needs
//     the WebhookThreadPool or any new threading infrastructure:
//     (1) `handoff` end-to-end: runAction's new Action::kHandoff case and
//     applyWebhookResponse's new `handoff` field both call the same
//     fireHandoff(reason) - increments handoff_count, then builds a
//     HandoffContext (session_id/reason/active_flow_id/active_node_id/a copy
//     of the current session parameters - fields spec's own §20.1 example
//     doesn't enumerate beyond sessionId/reason, but a handoff callback with
//     nothing but those two is of little use to an app actually routing to a
//     human agent) and invokes _onHandoff if one is attached (setOnHandoff
//     above) - same "increments the metric either way, callback is the only
//     optional part" pattern _webhookErrorCount already establishes.
//     (2) EventHandlers.no_match, spec §6 step 3's "the node's no-match
//     fallback (falling back to the flow's, then the agent's)" - the "not-yet-
//     scheduled gap" M2.5's own class comment flagged. evaluateRoutesOnce()
//     tracks whether any route actually fired across its three groups; if
//     none did *and* this is the ordinary step-3 entry point (eventName ==
//     nullptr - ordinary intent/condition evaluation immediately following
//     step 2, not an internally-cascaded node_enter/emitEvent pass, which
//     stays a silent no-op per the class comment further below), handleNoMatch()
//     runs findNoMatchHandler()'s resolved Executor (same node -> flow ->
//     agent chain shape as findWebhookErrorHandler, kept as a separate
//     near-duplicate rather than a shared helper - only two of these exist)
//     with event.attempts populated from _noMatchAttempts (spec §8.2's table:
//     "the reprompt count at the moment the fallback fired" - reset on
//     enterNode() like _generativeTurnsUsed, otherwise a historical count not
//     reset by an intervening successful match, matching ParameterMeta.
//     attempts' own precedent). No _abortExecutor involved here (unlike
//     handleWebhookFailure) - nothing is mid-executor at the point no_match
//     fires, since it's detected only after every route in the node has
//     already been tried and none matched.
//   - Deliberately NOT implemented this milestone, both explicitly later
//     items: the no-input half of EventHandlers (`no_input`, spec §9.5) -
//     still blocked on the same missing OrchestratorFrameProcessor-side
//     turn-detection integration M2.5's class comment already flagged, no
//     closer to existing now than it was then; and `permission_denied`/
//     `webhook_deferred`'s own trigger paths (the permission gate,
//     PendingActionStore respectively) - both remain Phase 4 roadmap items
//     with their own prerequisite machinery this milestone doesn't add, so
//     there is nothing yet that could ever resolve their EventHandlers chain
//     even if one were added. `user_cancel`'s own trigger path - interrupt
//     intents - is no longer in this category; see the §6.2 bullet earlier
//     in this comment and findUserCancelHandler()/handleUserCancel() below.
//   - Generative nodes (§9.4): while the active node has generative: true,
//     handleUtterance() skips NLUProvider.detect() and §6 steps 1-3 entirely
//     (a generative node never touches NLUProvider for the LLM turn itself -
//     though runGenerativeTurn() does now make one interrupt-only detect()
//     call ahead of it, spec §6.2, see the §6.2 bullet earlier in this
//     comment) and instead runs runGenerativeTurn(), which drives
//     _llmTransport directly - a system message
//     (generative_config.system_prompt_template, {param}-substituted like
//     say.text, plus a tool-calling contract this engine defines: reply with
//     only {"tool": "<name>", "args": {...}} to call one of
//     generative_config.tools, anything else is treated as plain spoken text)
//     installed once per node-entry, then one user-turn's worth of
//     tool-call/tool-result exchanges (each dispatched through the same
//     setCustomToolHandler registry an ordinary route's CustomToolAction
//     uses - spec §9.4 point 3's "the ordinary customTool handler") until the
//     model produces plain text (this turn's say output) or calls a
//     `terminal` tool, whichever ends the loop first and emits
//     generative_config.exit_event (default "generative_exit") as a pending
//     event carrying event.exit_reason ("terminal_tool" | "max_turns" |
//     "interrupt" - spec §8.2's table), re-entering ordinary route evaluation
//     at step 3 exactly like any other emitted event. _generativeTurnsUsed -
//     reset whenever any node is
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
//     SessionPersistence.h). Milestone 2.5 populates parameter_meta and
//     pending_ask on both sides of that round trip (SessionState now tracks
//     both - see its own class comment), since spec §9.5 explicitly requires
//     a mid-ask session to survive restore; route_history/event_counters
//     still travel empty (nothing in this milestone's scope needs them).
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
// steady state (waiting for the next input), not an error. Milestone 3.3
// implements no_match itself (see above) exactly along this boundary:
// evaluateRoutesOnce()'s eventName == nullptr check is what distinguishes
// "the ordinary step-3 pass this rule is about" from "an internal cascade
// this rule says stays silent."
//
// Milestone 1.8 (docs/reference/requestflow-roadmap.md §3) added spec §11's
// per-turn metrics into the existing MetricsCollector, via
// MetricsCollector::instance() (same default-singleton pattern as
// va::transport::BaseTransport - no injection setter, since spec §13 lists
// no setMetricsCollector on OrchestratorEngine). route_eval_latency_ms
// (Sampler, one observation per external turn - start() or handleUtterance() -
// covering that turn's whole route-eval-and-cascade work), route_match_count
// (Counter, once per route that actually fires), completion_count (Counter,
// once per END_SESSION transition), and turns_to_completion (Sampler, session
// turnCount at that same moment) are wired from M1.6/M1.7. Milestone 2.5 adds
// parameter_validation_failures (Counter, once per failed
// validateAndNormalize()/validateObject() call in
// handleParameterExtractionTurn() - both the static-form and the `expect`
// case share this one counter, since spec §11 doesn't split it by source).
// Milestone 3.1 adds webhook_latency_ms (Sampler, one observation per
// callWebhook action - the whole wait, including its one retry if any - not
// per individual HTTP attempt) and webhook_error_count (Counter, once per
// callWebhook action that ends in the webhook_error fallback, whether from
// exhausted retries or a turnExternalCallBudget expiry - not incremented for
// a circuit-breaker refusal, which throws before there's a "webhook_error
// outcome" to count). Milestone 3.3 adds handoff_count (Counter, once per
// fireHandoff() call - a HandoffAction or a webhook response's `handoff`
// field, whether or not setOnHandoff is attached) and no_match_count
// (Counter, once per handleNoMatch() call - see the class comment above).
// no_input_count still needs the no_input half of EventHandlers, still
// blocked on the same missing turn-detection integration M2.5 flagged;
// nlu_*/fire_and_forget_*/best_effort_* need Phase 3/4 features this engine
// doesn't implement yet - wiring them now would mean a counter that can
// never increment, which is worse than not registering it at all.
class VASDK_EXPORT OrchestratorEngine {
public:
    explicit OrchestratorEngine(va::orchestrator::Agent agent, OrchestratorConfig config = {});

    // Milestone 3.1 - declared (rather than implicitly defaulted) because
    // _webhookThreadPool is a unique_ptr<WebhookThreadPool>, and this header
    // only forward-declares WebhookThreadPool; the implicit destructor would
    // need its complete definition right here to compile. Defined `= default`
    // in OrchestratorEngine.cpp, after that class's full definition.
    ~OrchestratorEngine();

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

    // Milestone 2.5 (spec §7) - passthrough to the engine's own
    // ParameterValidator, for a static-form or `expect` Parameter naming this
    // `name` as its validator_ref. Optional: a Parameter that never sets
    // validator_ref never needs one; one that does and finds nothing
    // registered throws std::runtime_error at the point it's evaluated (see
    // ParameterValidator::validateAndNormalize), the same "fail loudly"
    // treatment every other missing-wiring case in this class already gets.
    void registerParameterValidator(const std::string& name, ParameterValidator::CustomValidator fn);

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

    // Milestone 3.1 (spec §9.2) - attaches the HttpInterface callWebhook
    // actions dispatch through (off the calling thread - see class comment).
    // Unset is not a supported configuration for an agent that actually
    // reaches a callWebhook action: it throws at that point, the same
    // "wiring never completed" treatment setLLMTransport's own missing case
    // already gets. Inject a fake implementation here for deterministic,
    // no-live-endpoint tests (spec §16).
    void setHttpInterface(std::shared_ptr<va::network::HttpInterface> http);

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

    // Milestone 3.3 (spec §13/§20.1) - fires once per HandoffAction that runs
    // (runAction's Action::kHandoff case) or webhook response `handoff` field
    // (requestflow-backend-spec.md §3) that's applied. Unset is a fully
    // supported shape, same as setOnFlowComplete - handoff_count still
    // increments either way (see class comment), it's only the app-visible
    // callback that's a no-op.
    void setOnHandoff(std::function<void(const va::orchestrator::HandoffContext&)> cb);

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

    // Milestone 2.5 - reset at the top of every start()/handleUtterance()
    // call; set true the instant an `expect` action runs (runAction's
    // Action::kExpect case). See the class comment above for every place this
    // is checked to actually stop the rest of that call's processing.
    bool _suspendTurn = false;

    // Milestone 3.1 - reset at the top of every start()/handleUtterance()
    // call alongside _suspendTurn; set true by handleWebhookFailure(). Unlike
    // _suspendTurn, always consumed-and-reset the instant runExecutor()/
    // fireRoute()/enterNode() sees it - see class comment.
    bool _abortExecutor = false;

    // Milestone 3.1 - the current external turn's utterance, if any (empty
    // for a start()-triggered turn); requestflow-backend-spec.md §2's
    // `utterance` request field.
    std::string _lastUtterance;

    std::deque<PendingEvent> _pendingEvents;
    std::unordered_map<std::string, ExpressionEvaluator> _compiledExpressions;

    // Milestone 2.5 (spec §7) - backs both static-form and `expect` Parameter
    // validation; registerParameterValidator() above is the only way an app
    // adds a validator_ref entry to it.
    ParameterValidator _parameterValidator;

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
    // Milestone 3.3 - see setOnHandoff above.
    std::function<void(const va::orchestrator::HandoffContext&)> _onHandoff;

    // Milestone 3.3 (spec §6 step 3/§8.2) - consecutive no-match fallback
    // firings on the *current* node, reset on enterNode() the same way
    // _generativeTurnsUsed is - a historical count, not reset by an
    // intervening successful match, matching ParameterMeta.attempts' own
    // "historical failed-attempt count" precedent. Surfaced as event.attempts
    // per spec §8.2's table.
    int _noMatchAttempts = 0;

    // Milestone 1.8 (spec §11) - see the class comment above for exactly
    // which four metrics these are and why the rest aren't registered yet.
    va::metrics::SamplerHandle _routeEvalLatencyMs;
    va::metrics::CounterHandle _routeMatchCount;
    va::metrics::CounterHandle _completionCount;
    va::metrics::SamplerHandle _turnsToCompletion;
    va::metrics::CounterHandle _parameterValidationFailures;
    // Milestone 3.1 (spec §11) - see the class comment above.
    va::metrics::SamplerHandle _webhookLatencyMs;
    va::metrics::CounterHandle _webhookErrorCount;
    // Milestone 3.3 (spec §11) - see the class comment above.
    va::metrics::CounterHandle _handoffCount;
    va::metrics::CounterHandle _noMatchCount;
    void initializeMetrics();

    // Milestone 3.1 (spec §9.2) - see the class comment above. _httpInterface
    // unset + a reached callWebhook action throws, same "wiring never
    // completed" treatment as a missing LLMTransport. _webhookCircuits is
    // keyed by CallWebhookAction.url; kCircuitBreakerFailureThreshold/
    // kCircuitBreakerCooldown are defined next to their one use in
    // OrchestratorEngine.cpp. _webhookThreadPool is declared last in this
    // class deliberately - reverse member-destruction order means it's torn
    // down (draining/joining every in-flight task) *before* _httpInterface,
    // _session, _agent, or anything else a still-running task's captured
    // lambda might dereference.
    std::shared_ptr<va::network::HttpInterface> _httpInterface;
    struct WebhookCircuitState {
        int consecutiveFailures = 0;
        std::chrono::steady_clock::time_point openUntil{};
    };
    std::unordered_map<std::string, WebhookCircuitState> _webhookCircuits;

    void compileAllExpressions();
    const ExpressionEvaluator& compiledExpression(const std::string& text) const;

    const va::orchestrator::Flow* findFlow(const std::string& flowId) const;
    const va::orchestrator::Node* findNode(const va::orchestrator::Flow& flow, const std::string& nodeId) const;
    const va::orchestrator::Node* currentNode() const;

    // Deduped, declaration-order IntentTrigger.intent_name values across
    // `node`'s intent-triggered routes - spec §6 step 2's "candidateIntentNames
    // = node routes".
    std::vector<std::string> intentCandidateNames(const va::orchestrator::Node& node) const;

    // Spec §6.2 - Agent.interrupt_intent_names ∪ (active flow's) ∪
    // (node's), deduped, agent-first order. Populated into
    // NLUContext.interrupt_intent_names on every detect() call (see class
    // comment above) and re-derived here (not cached) to check
    // _lastNluResult.top_intent() membership after the fact - both
    // evaluateRoutesOnce() and the two handleUtterance()/
    // handleParameterExtractionTurn() call sites need it independently, and
    // it's cheap enough (three short repeated-field scans) not to bother
    // threading a precomputed copy between them.
    std::vector<std::string> mergedInterruptIntentNames(const va::orchestrator::Node& node) const;

    ExpressionEvaluator::Activation buildActivation(const google::protobuf::Struct* eventParams) const;
    std::string substituteParameters(const std::string& text) const;

    void enterNode(const std::string& flowId, const std::string& nodeId);
    // Milestone 3.1 - routeId is new (see class comment): the one piece of
    // per-call context callWebhook's request body needs that nothing before
    // this milestone required. Empty from enterNode (no matched route) and
    // from handleWebhookFailure's own fallback-executor run.
    void runExecutor(const va::orchestrator::Executor& executor, const google::protobuf::Struct* eventParams,
                      const std::string& routeId);
    void runAction(const va::orchestrator::Action& action, const google::protobuf::Struct* eventParams,
                    const std::string& routeId);
    void applyTransition(const va::orchestrator::Transition& transition);

    // Milestone 2.4 (spec §9.6) - looks up and invokes a registered
    // customTool handler; throws if `toolName` was never registered (see
    // class comment).
    google::protobuf::Struct callCustomTool(const std::string& toolName, const google::protobuf::Struct& args);

    // Milestone 2.4 (spec §9.4) - drives one external turn of a generative
    // node's LLM-tool loop against `node`. See the class comment above for
    // the full mechanics. `injectedResult` (spec §6.2, added alongside the
    // interrupt-intents milestone) is the same simulator convention
    // handleUtterance()/handleParameterExtractionTurn() already use - it
    // stands in for the new interrupt-check prelude's detect() call, letting
    // a scripted test drive an interrupt mid-generative-loop without a real
    // NLUProvider attached; nullptr means "call the real provider" as usual.
    void runGenerativeTurn(const va::orchestrator::Node& node, const std::string& utterance,
                            const va::orchestrator::NLUResult* injectedResult);
    std::string buildGenerativeSystemPrompt(const va::orchestrator::GenerativeConfig& config) const;

    // Returns true when this evaluation pass should stop (a transition fired,
    // the session ended, the turn was suspended by an `expect` action, or the
    // matched route's stop_evaluation - default true - says so).
    bool fireRoute(const va::orchestrator::Route& route, const google::protobuf::Struct* eventParams);
    void evaluateRoutesOnce(const std::string* eventName, const google::protobuf::Struct* eventParams);
    void runToQuiescence();

    // Milestone 2.5 (spec §9.5/§7) - drains pending events (runToQuiescence())
    // and then, unless the turn already ended (_ended) or was suspended by an
    // `expect` action (_suspendTurn), calls checkFormParameterAsk() for
    // whichever node the cascade settled on. Every runToQuiescence() call site
    // in this class except the two mid-reprompt-failure early returns in
    // handleParameterExtractionTurn() goes through this instead.
    void finishTurn();

    // First required (per ParameterValidator::isRequired), currently-unfilled
    // (per SessionState::getParameter) Parameter in `node.form()`, in
    // declaration order; nullptr if none. Spec §7's "one at a time" static
    // form collection.
    const va::orchestrator::Parameter* findUnfilledFormParameter(const va::orchestrator::Node& node) const;

    // Spec §19's collect_reason/collect_date worked example: the moment a
    // node settles with a required, unfilled form parameter never asked
    // before, says its `prompts.initial` once (SessionState::
    // noteParameterAskStarted marks it asked). A no-op for a generative node,
    // a node with no such parameter, or one already mid-ask.
    void checkFormParameterAsk();

    // Milestone 2.5 (spec §6 step 2/§9.5) - the "a parameter is being asked
    // for" turn: `parameter` is either a PendingAsk's ephemeral Parameter or a
    // static-form one (see class comment above for the full priority chain
    // and mechanics). Ends the turn either via a reprompt/escalation `say` (no
    // route evaluation) or by validating, storing, and falling through to an
    // ordinary step-3 evaluation + finishTurn().
    void handleParameterExtractionTurn(const va::orchestrator::Parameter& parameter, const std::string& text,
                                        const va::orchestrator::NLUResult* injectedResult);

    // Milestone 3.1 (spec §9.2) - dispatches `webhook` (routeId - see
    // runExecutor above - for the request body's routeId field) to
    // _webhookThreadPool, waits bounded by turnExternalCallBudget, and either
    // applies a successful JSON response (applyWebhookResponse) or runs the
    // webhook_error fallback chain (handleWebhookFailure). Throws if
    // _httpInterface is unset or this endpoint's circuit breaker is open -
    // see class comment.
    void handleWebhookAction(const va::orchestrator::CallWebhookAction& webhook, const std::string& routeId);

    // Runs findWebhookErrorHandler()'s resolved Executor (if any) with
    // event.status_code/error_message/url populated, then sets
    // _abortExecutor - spec §9.2's "re-enter route evaluation at step 3 so
    // EventHandlers.webhook_error... decides what happens next." See class
    // comment for why this isn't an ordinary emitted/pending event.
    void handleWebhookFailure(const std::string& url, int statusCode, const std::string& errorMessage);

    // requestflow-backend-spec.md §3 - parses `rawJsonBody` and applies
    // `messages`/`parameterPatches`/`transition`/`emitEvents`/`endSession` in
    // that order (`handoff` intentionally not implemented yet - see class
    // comment). Malformed or non-object JSON is a no-op, same as an
    // explicitly empty `{}` response.
    void applyWebhookResponse(const std::string& rawJsonBody);

    // node_handlers -> flow_handlers -> global_handlers, first one whose
    // has_webhook_error() is true; nullptr if none anywhere in that chain.
    const va::orchestrator::Executor* findWebhookErrorHandler() const;

    // Milestone 3.3 (spec §6 step 3) - same node -> flow -> agent chain as
    // findWebhookErrorHandler, but for EventHandlers.no_match; kept as a
    // separate near-duplicate rather than a shared member-pointer-templated
    // helper, since there are only two of these and the duplication is small.
    const va::orchestrator::Executor* findNoMatchHandler() const;

    // Milestone 3.3 (spec §6 step 3/§8.2) - runs findNoMatchHandler()'s
    // resolved Executor (if any) with event.attempts populated
    // (_noMatchAttempts, pre-increment), then increments no_match_count.
    // Called from evaluateRoutesOnce() when the ordinary step-3 pass
    // (eventName == nullptr) matches no route at all - an internally-cascaded
    // event matching nothing stays a silent no-op (see class comment above).
    void handleNoMatch();

    // Spec §6.2 - same node -> flow -> agent chain as findWebhookErrorHandler/
    // findNoMatchHandler, but for EventHandlers.user_cancel; a third
    // near-duplicate rather than a shared helper, same "only a few of these
    // exist" reasoning findNoMatchHandler's own comment already gives.
    const va::orchestrator::Executor* findUserCancelHandler() const;

    // Spec §6.2's fallback half of the interrupt-intents mechanism: called
    // from evaluateRoutesOnce() in place of handleNoMatch() when the ordinary
    // step-3 pass matches no route at all (anyRouteFired stays false) *and*
    // this turn's winning top_intent is itself an interrupt match - i.e. the
    // author wrote no ordinary intent-triggered Route to catch it, so
    // EventHandlers.user_cancel is the resolved trigger path spec §18 flagged
    // as an open decision in Draft v2.1 and resolved in Draft v2.2. Unlike
    // handleNoMatch(), no event.* payload is documented for this fallback
    // (spec §8.2's table has no row for it) - runs findUserCancelHandler()'s
    // resolved Executor (if any) with no eventParams.
    void handleUserCancel();

    // Milestone 3.3 (spec §13/§20.1) - the shared path both a HandoffAction
    // (runAction's Action::kHandoff case) and a webhook response's `handoff`
    // field (applyWebhookResponse) fire through: increments handoff_count,
    // builds a HandoffContext from current session state, and invokes
    // _onHandoff if one is attached.
    void fireHandoff(const std::string& reason);

    // See the class comment above for why this is declared last: reverse
    // member-destruction order tears it down - draining/joining every
    // in-flight task - before any other member a queued task's captured
    // lambda could still dereference.
    std::unique_ptr<WebhookThreadPool> _webhookThreadPool;
};

} // namespace orchestrator
} // namespace va
