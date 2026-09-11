# RequestFlow Implementation Roadmap

Reference target: the ticket-sized milestone breakdown for actually building `va::orchestrator` - who builds what, in what order, and what "done" looks like for each piece. This is the implementer-facing companion to [requestflow-spec.md §15](requestflow-spec.md#15-rollout-plan): §15 stays the authoritative phase *summary* (and the source of truth if this document ever drifts from it); this document exists because §15's Phase 4 in particular bundles roughly fifteen independently-shippable features into one paragraph, which isn't small enough to plan or staff against.

Status: **Draft v1.** Written before any `va::orchestrator` code exists - see the note in [§0](#0-current-state).

## 0. Current State

M0 (module scaffolding), M1.1 (proto schema/codegen), M1.2 (`OrchestratorConfigLoader` + `validate()`), and M1.3 (`PhraseGrammarMatcher`, the §22.1 phrase-grammar compiler) are done and committed. M1.4 as originally written here as M1.3 (CEL integration) is still blocked on a real gap discovered during M0: `cel-cpp` has no CMake build (Bazel-only), contradicting [§0.1](requestflow-spec.md#01-what-changed-in-draft-v21)'s "one-target-per-dependency `FetchContent` convention" claim - deferred rather than guessed at, see the M0 commit. §22.1's phrase grammar was redesigned in [Draft v2.9](requestflow-spec.md#09-what-changed-in-draft-v29) (unified `{{name:type}}` capture, `(a|b|c)` alternation) specifically because it has no such blocker - pure hand-rolled parsing, zero external dependencies - which is why it was pulled forward into Phase 1 ahead of CEL. The one thing that *is* already true and worth reusing rather than rediscovering: `VASDK/src/goal/` and `VASDK/include/VASDK/goal/` are the closest existing analog (slot validation, a conversation simulator, three-method persistence, an intent router) and [requestflow-spec.md §3](requestflow-spec.md#3-relationship-to-existing-vasdk-components) already specifies exactly what's ported wholesale versus built fresh - Milestone 0 leaned on that directly.

## 1. How to Read This

- Each milestone is scoped to be independently shippable and CI-testable - via `OrchestratorConversationSimulator` once it exists, via a throwaway unit test before it does - mirroring `goal/`'s existing test posture rather than introducing a new one.
- **Phase 1 milestones are strictly ordered** (each genuinely depends on the previous one existing). **Phase 4 milestones are explicitly *not* ordered relative to each other** - §4 below replaces §15's single "Phase 4" bullet with one row per feature and a rationale for *when* you'd reach for it, so the choice of what to build next is driven by which concrete vertical needs it, not by document order.
- Every milestone cites its `requestflow-spec.md`/`requestflow-backend-spec.md` section(s) - this document defines sequencing and done-criteria, never new mechanism. If a milestone's behavior isn't already specified in one of those two files, it isn't ready to schedule yet; go write that section first.

## 2. Milestone 0 - Module Scaffolding (prerequisite to everything)

Not a phase from §15 - nothing in Phase 1 can start without it, and it's small enough to be its own milestone rather than folded into M1.1.

- New `va::orchestrator` module: `VASDK/src/orchestrator/`, `VASDK/include/VASDK/orchestrator/`, one class per file, mirroring `VASDK/src/goal/`'s existing layout exactly (confirmed present: `GoalDefinitionLoader`, `GoalManager`, `GoalState`, `GoalSlot`, `ConditionEvaluator`, `IntentRouter`, `GoalConversationSimulator`, `GoalStatePersistence` - each a `.h`/`.cpp` pair).
- A new CMake option gating the whole module (e.g. `VASDK_BUILD_ORCHESTRATOR`, default `OFF` until Phase 1 is usable), following the exact pattern the top-level `CMakeLists.txt` already uses for `VASDK_BUILD_CALLCENTER`/`VASDK_ENABLE_HIGHWAY`.
- `FetchContent` for `protobuf` and `cel-cpp`, each pulled and gated behind that same option, per [requestflow-spec.md §0.1](requestflow-spec.md#01-what-changed-in-draft-v21)/[§3](requestflow-spec.md#3-relationship-to-existing-vasdk-components)'s "one-target-per-dependency convention as every existing provider."
- **Done when**: an empty `va::orchestrator` target builds clean with the option `ON`, and the rest of VASDK - `goal/`, the call center showcase, everything else - builds clean and unaffected with the option `OFF` (the default). Zero impact on any existing target is the acceptance bar here, not orchestrator functionality.

## 3. Phase 1 Milestones - Deterministic Core (strictly ordered)

Mirrors [requestflow-spec.md §15](requestflow-spec.md#15-rollout-plan) Phase 1, broken into buildable/testable steps.

**M1.1 - Proto schema.** Author the `.proto` schema from [§4](requestflow-spec.md#4-domain-model) (`Agent`/`Flow`/`Node`/`Route`/`Trigger`/`Parameter`/`Executor`/`Action`/`Transition`/`SessionSnapshot` and friends), generate C++ types via `protoc`, wire codegen into CMake. *Done when*: the schema compiles and a throwaway test can construct and serialize/deserialize an `Agent` message.

**M1.2 - Config loader + offline validator.** `OrchestratorConfigLoader::fromTextprotoFile`/`fromBinaryFile`/`fromDirectory` (including the [§14.1](requestflow-spec.md#141-splitting-config-across-files) multi-file composition rule), and `validate()` covering referential integrity, reachability, handler coverage, and the [§9.5](requestflow-spec.md#95-expect-and-inline-slot-elicitation) dead-end-node check. *Done when*: the [§14](requestflow-spec.md#14-config-format-example-textproto) `appointments-agent` example loads and validates clean, and a small set of deliberately-broken fixtures (bad `target_id`, an unreachable node, a dead-end node with no transition/event/turn-spanning mechanism) each fail `validate()` with a distinct, attributable error.

**M1.3 - Phrase-grammar compiler.** A standalone parser + matcher for [§22.1](requestflow-spec.md#221-phrase-grammar)'s phrase grammar (Draft v2.9: `{{name:type}}` capture, `(a|b|c)` alternation, `*`/`.` wildcards, uniform postfix `?`, bare `{canonical}`, leading `!` negative examples). Compiles `TrainingSet.intents[].phrases` into a matcher at load time - malformed phrase syntax, an unresolvable `type`, or a capture name reused across alternation branches all fail at compile time, never at match time - and matches an utterance against it, returning candidate intents with `extracted_parameters` and a specificity-based confidence score. No engine, no `NLUProvider` interface, no CEL dependency: pure string-in/matches-out, independently testable, and unblocked by the M1.4 CEL question below. *Done when*: [§22.4](requestflow-spec.md#224-worked-example)'s rewritten worked examples (volume-control alternation, climate-control zone+delta) match their documented example utterances with the documented `extracted_parameters`, and each of the deliberately-malformed phrase cases above fails compilation with a distinct, attributable error.

**M1.4 - CEL integration.** Wire `cel-cpp` for `condition_cel`/`event` triggers and `value_cel`/`args_cel`, compiled once at load time against session `parameters` plus the well-known counters (`turnCount`, `repromptCount`, `sessionElapsedMs`). `nlu.*`/`context.*`/`shared.*`/`event.*`/`entity.*` are later milestones (§5 below) - this milestone only needs the base activation. Still blocked on the `cel-cpp`/CMake build-system gap noted in [§0](#0-current-state) as of this writing - resolve that before scheduling this milestone. *Done when*: every expression in [§8](requestflow-spec.md#8-condition--expression-language)'s worked examples evaluates correctly against a fixture session, and a malformed expression fails at load time, not at evaluation.

**M1.5 - Parameter model.** Port `GoalSlot`'s validation/normalization semantics ([§7](requestflow-spec.md#7-parameter-model-and-collection-policy)) into `Parameter`: `STRING`/`PHONE`/`EMAIL`/`DATE`/`TIME`/`DATETIME`/`NUMBER`/`INTEGER`/`BOOLEAN`/`ENUM`/`OBJECT`/`COMPOSITE`, plus the `condition_cel`/`validator_ref` escape hatches. *Done when*: `goal/`'s existing slot-validation test cases produce identical pass/fail/normalized-value results run against the ported implementation - this is a port, so a behavioral diff is a bug, not a design choice.

**M1.6 - Engine + processor, event/condition routes only.** `OrchestratorEngine` and `OrchestratorFrameProcessor` implementing [§6](requestflow-spec.md#6-route-evaluation-semantics) steps 3-6 for `event` and `condition` routes only - no `intent` routes yet, since those need `NLUProvider` (Phase 2). Includes the live `SessionState` object ([§13](requestflow-spec.md#13-public-api-surface-proposed-c)). *Done when*: a config containing only `event`/`condition` routes runs end-to-end through a real `Pipeline` in a test - `node_enter` fires, transitions apply, executor actions (`say`/`setParameter`/`clearParameter`/`emitEvent`) run in order.

**M1.7 - Session persistence.** `SessionSnapshot` proto (already defined in M1.1) plus `FileSessionPersistence` - binary by default, `TextFormat` in a debug mode - per [§10](requestflow-spec.md#10-session-state-and-persistence). *Done when*: a session snapshotted mid-flow and restored has identical `parameters`/`active_node_id` - this is one of [§16](requestflow-spec.md#16-acceptance-criteria-v1)'s acceptance criteria directly, so it doubles as a v1 sign-off check.

**M1.8 - `OrchestratorConversationSimulator` + metrics.** Mirrors `GoalConversationSimulator`'s scripted-turn design; wires [§11](requestflow-spec.md#11-observability)'s per-turn metrics into `MetricsCollector`/`CallMetrics`. *Done when*: every [§16](requestflow-spec.md#16-acceptance-criteria-v1) acceptance criterion that doesn't require NLU or webhooks is green in CI through the simulator, with no live model, webhook, or audio hardware.

**Phase 1 exit criterion** (= [requestflow-spec.md §15](requestflow-spec.md#15-rollout-plan)'s Phase 1 boundary): a build with zero `LLMProcessor`/`ToolDispatchProcessor` loads a real multi-flow, multi-file config and runs `event`/`condition`-only routes end-to-end in CI.

## 4. Phase 2-3 Milestones - Understanding, Then External Actions

Also strictly ordered within each phase; Phase 3 depends on Phase 1's engine but not on Phase 2's NLU (a build can add webhooks to an event/condition-only agent).

**Phase 2 - Understanding**
- **M2.1** - `NLUProvider` interface + `TrainingPhraseNLUProvider`, wrapping M1.3's already-built phrase-grammar compiler behind the polymorphic interface ([§5](requestflow-spec.md#5-nluprovider-interface)/[§22](requestflow-spec.md#22-nlu-training-data)). Ship this before the LLM-backed provider - it's the "LLM-optional by construction" claim in [§2](requestflow-spec.md#2-design-principles), and proving it first means the LLM-backed provider (M2.2) is demonstrably swappable rather than load-bearing.
- **M2.2** - LLM-tool-backed `NLUProvider`, structured classification call against `LLMTransport`. *Done when*: swapping M2.1 for M2.2 behind the interface requires zero changes to `OrchestratorEngine`/`OrchestratorFrameProcessor` - another direct [§16](requestflow-spec.md#16-acceptance-criteria-v1) criterion.
- **M2.3** - `intent` routes wired into [§6](requestflow-spec.md#6-route-evaluation-semantics) step 2/3, using either provider.
- **M2.4** - `llmGenerate` executor action ([§9.3](requestflow-spec.md#93-llmgenerate-and-barge-in)) and generative nodes ([§9.4](requestflow-spec.md#94-generative-nodes)) - the scoped LLM-tool loop.
- **M2.5** - `expect` executor actions, typed and bare ([§9.5](requestflow-spec.md#95-expect-and-inline-slot-elicitation)) - depends on M2.1-M2.3's extraction path, which is why §15 places it in Phase 2 rather than Phase 1 alongside the rest of the `Action` oneof.

**Phase 3 - External actions and hardening**
- **M3.1** - `callWebhook` with the full [§9.2](requestflow-spec.md#92-callwebhook-execution-model) reliability model: timeout, one retry on transient failures, per-endpoint circuit breaker, idempotency key.
- **M3.2** - A reference backend conformance pass against [requestflow-backend-spec.md](requestflow-backend-spec.md): implement one language's stub (Spring Boot or FastAPI, per [§8](requestflow-backend-spec.md#8-language-implementation-guides)) and verify the engine's M3.1 output against it end-to-end, including the HMAC signing ([§6](requestflow-backend-spec.md#6-authentication)) and idempotent-replay ([§5](requestflow-backend-spec.md#5-idempotency)) behavior. This is also the natural point to build the fixture set proposed in [requestflow-backend-spec.md §10](requestflow-backend-spec.md#10-open-decisions) - see §6 below, it doesn't have to wait for M3.2, but M3.2 is where the fixtures get proven against a real engine rather than only a mock.
- **M3.3** - `handoff`/escalation event handlers end-to-end.
- **M3.4** - Cross-flow transition edge cases: `preserveParameters` interactions, mid-transition barge-in.

**Phase 2/3 exit criterion**: [§16](requestflow-spec.md#16-acceptance-criteria-v1)'s remaining acceptance criteria (generative node with bounded exit, webhook retry/circuit-breaker under simulated failure) are green in CI.

## 5. Phase 4 Milestones - Cross-Vertical Extensions (build on demand, not in document order)

[§15](requestflow-spec.md#15-rollout-plan)'s Phase 4 paragraph lists roughly fifteen features from Draft v2.2 through v2.8 as one undifferentiated block. They share nothing structurally that forces an order - each is additive and independently testable against the Phase 1-3 core. The table below is meant to be scheduled by vertical need, matching the philosophy [§0.2](requestflow-spec.md#02-what-changed-in-draft-v22) already states for how these were scoped in the first place: build a feature when a concrete deployment target needs it, not speculatively.

| Feature | Spec section | Reach for it when... |
| --- | --- | --- |
| `TriggerFrame` consumption, ambient entry, proactive speech | [§9.7](requestflow-spec.md#97-external-and-scheduled-triggers) | Any deployment needs a flow to start from something other than user speech (a scheduled reminder, a sensor, a companion-app button) - agriculture and homelab both need this early. |
| Multi-intent co-firing | [§6.1](requestflow-spec.md#61-multi-intent-co-firing) | A vertical's utterances commonly bundle two independently-actionable requests ("play jazz and turn up the AC") - automotive-shaped. |
| Interrupt intents / mid-task cancel | [§6.2](requestflow-spec.md#62-interrupt-intents-and-mid-task-cancel) | Any vertical with slot-filling or generative nodes long enough that a user plausibly wants to bail mid-way - build this alongside the first generative node (M2.4) or the first multi-parameter form, whichever ships first. |
| Permission gate (`required_role`, `speaker_id`) | [§9.8](requestflow-spec.md#98-permission-gate) | A route gates something consequential enough to need an actor check (chemical application, door unlock) - agriculture/homelab. |
| `context.*`/`shared.*` namespaces | [§8.1](requestflow-spec.md#81-external-context-and-shared-state-namespaces) | Routes need continuously-changing external facts (`context.*` - vehicle speed, time of day) or cross-engine shared facts (`shared.*` - whole-home state). These are two separate milestones even though one section covers both, since they have different write semantics and no dependency on each other. |
| `event.*` namespace on `EmitEventAction` | [§8.2](requestflow-spec.md#82-the-event-namespace) | Natural to build alongside `TriggerFrame` consumption (its payload rides this namespace) or whenever the first author wants to pass data through an emitted event rather than just a bare name. |
| Offline-resilient `callWebhook` (`PendingActionStore`) | [§9.9](requestflow-spec.md#99-offline-resilient-callwebhook-store-and-forward) | A deployment has real multi-hour connectivity gaps - agriculture, specifically; automotive/homelab likely never need this. |
| Audit sink | [§11.1](requestflow-spec.md#111-audit-sink-for-consequential-actions) | Alongside the permission gate, for the same consequential-action deployments. |
| Hot-reload (`reloadAgent`) | [§13.1](requestflow-spec.md#131-hot-reloading-a-running-agent) | A homelab-style author wants to iterate on a live config without dropping the session - low priority unless that workflow is a near-term target. |
| Typed `customTool` codegen (`ToolSchema`) | [§23](requestflow-spec.md#23-typed-customtool-argument-and-result-codegen) | Once a vertical accumulates enough hand-written `customTool` handlers that a field-name typo has actually caused a production bug - build reactively, not ahead of the pain. |
| Typed `context.*` codegen (`ContextSchema`) | [§24](requestflow-spec.md#24-typed-context-codegen) | Immediately after `context.*` itself (above) ships, if that vertical's context provider is non-trivial enough to want compile-time field checking. |
| Entity-backed CEL constants (`entity.*`) | [§25](requestflow-spec.md#25-entity-backed-cel-constants) | Once `entities.textproto` ([§22](requestflow-spec.md#22-nlu-training-data)) exists for a vertical and routes are already retyping the same canonical values as CEL string literals. |
| Intent capture defaults | [§26](requestflow-spec.md#26-default-values-for-intent-captures) | Once a vertical's routes are visibly repeating the `has(nlu.x) ? nlu.x : default` ternary §26's own rationale describes - a code-smell trigger, not a calendar one. |
| Per-action `ExecutionMode` (`FIRE_AND_FORGET`/`BEST_EFFORT`) | [§9.10](requestflow-spec.md#910-action-execution-modes) | A vertical has a genuine fire-and-forget or best-effort use case (an analytics beacon, a non-critical preference sync) - otherwise `BLOCKING` (already in M1.6/M3.1) covers everything. |
| YAML/DSL-to-textproto authoring convenience | [§18](requestflow-spec.md#18-open-decisions) (first bullet) | Once `Parameter.constraints`/`EntityDictionary` hand-authoring is a measured pain point across two or more verticals, not before - this is tooling investment, justified by evidence, not a checklist item. |

Full management-plane versioning/environments and full RBAC ([§17](requestflow-spec.md#17-explicitly-out-of-scope-for-v1)) stay out of this roadmap entirely - they aren't Phase 4 items, they're explicitly deferred past v1, and nothing above depends on them.

## 6. Integrator-Facing Track - Parallel, Not Gated on Engine Code

[requestflow-backend-spec.md](requestflow-backend-spec.md)'s wire contract is plain JSON over HTTP and doesn't depend on `va::orchestrator` existing at all ([§0](requestflow-backend-spec.md#0-purpose-and-relationship-to-requestflow) is explicit about this). Everything here can start on day one, in parallel with Milestone 0 above, by a different person or team:

- **The OpenAPI document** proposed in [requestflow-backend-spec.md §10](requestflow-backend-spec.md#10-open-decisions) - request/response body schema, status-code contract, HMAC `securityScheme`, generated per `contractVersion`.
- **The conformance fixture set + CLI/script** also proposed there - the four fixture cases (happy path, not-found, malformed `contractVersion`, replayed idempotency key) plus a runner that POSTs them against a target URL.
- **A reference backend implementation** in one language (Spring Boot or FastAPI are the two with full code samples already in [§8](requestflow-backend-spec.md#8-language-implementation-guides)) as a working, clonable example - this is what M3.2 above ends up validating against, so building it early de-risks M3.2 rather than blocking on it.
- **The dev-tooling items** added to [requestflow-spec.md §18](requestflow-spec.md#18-open-decisions) in this same review pass (standalone validate/simulate CLI, `ValidationReport` diagnostic quality, route-trace/session-inspection tooling) *do* depend on Phase 1 existing (M1.2's `validate()`, M1.8's simulator) - these are listed here only as a pointer, not as parallelizable work.

## 7. Suggested Sequencing

- **Single track**: Milestone 0 → Phase 1 (strict) → Phase 2 → Phase 3 → Phase 4 (picked by vertical need, per §5's table).
- **Two tracks**: Track A runs the engine sequence above; Track B starts §6's integrator-facing work immediately and feeds M3.2 when Track A reaches it. This is the natural split if there's ever more than one person on this - the two tracks don't contend for the same files until M3.2.
- Whichever concrete vertical (automotive HVAC, agriculture, homelab) is the actual first target should be picked *before* Phase 4 starts, not after - it's what makes §5's table's "reach for it when" column resolve to a concrete, ordered list instead of staying a menu.

## 8. Definition of Done

The project-wide bar stays [requestflow-spec.md §16](requestflow-spec.md#16-acceptance-criteria-v1) - every milestone above cites which piece of §16 it contributes to specifically so that "is v1 done" never depends on re-deriving it from scratch.
