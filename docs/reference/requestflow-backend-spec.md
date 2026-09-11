# RequestFlow Backend & Webhook Authentication Spec

Reference target: the wire contract between a RequestFlow `OrchestratorEngine` ([requestflow-spec.md](requestflow-spec.md)) and an integrator's own backend service - the thing on the other end of every `CallWebhookAction`.

Status: **Draft v1.**

## 0. Purpose and Relationship to RequestFlow

[requestflow-spec.md §9.2](requestflow-spec.md#92-callwebhook-execution-model) defines `callWebhook` from the engine's side: when it fires, its retry/circuit-breaker/timeout behavior, and how a failure re-enters route evaluation. That's enough to understand the *engine*, but it deliberately doesn't spell out the wire format in enough detail for someone who has never read a line of the C++ spec to implement a conforming backend. This document is that other half: the request/response JSON schema, authentication, idempotency, timeout budgets, and per-language implementation guidance (Spring Boot, Python, Go, and how to extend to any other stack).

**Audience.** A backend developer implementing webhook endpoints for a RequestFlow agent. They should not need to read `requestflow-spec.md` at all to use this document, though [§9.2](requestflow-spec.md#92-callwebhook-execution-model) is cited throughout for anyone who wants the engine-side rationale.

**Why JSON, not protobuf.** RequestFlow's config, NLU boundary, and session persistence are all protobuf ([requestflow-spec.md §0.1](requestflow-spec.md#01-what-changed-in-draft-v21)). The webhook wire contract is the one deliberate exception, staying plain JSON over HTTP, because forcing an integrator's own REST backend to adopt protobuf would be a much bigger imposition than adopting it for VASDK's own config - and a REST backend in any of the stacks this document covers almost certainly already speaks JSON natively.

## 1. Scope

**In scope:** the request/response JSON schema; HTTP-level contract (methods, status codes, timeouts); idempotency; request authentication; contract versioning; implementation guidance for Java/Spring Boot, Python, and Go; conformance-testing guidance.

**Out of scope:**
- The C++ engine's internal retry/circuit-breaker mechanics - see [requestflow-spec.md §9.2](requestflow-spec.md#92-callwebhook-execution-model).
- A config-driven code generator that scaffolds backend stubs per language from an `Agent` config. This document defines the contract such a generator's output must satisfy; the generator itself is tracked as a related but separate initiative ([§10](#10-open-decisions)).
- Transport-level security beyond what [§6](#6-authentication) specifies (TLS termination, network policy, mTLS) - assumed to be handled by whatever's already in front of the backend (a load balancer, an API gateway, VASDK's own `HttpInterface`/`CurlHttpProvider` configuration).

## 2. The Webhook Request

Sent by the engine as the body of the HTTP call `CallWebhookAction` configures (`url`, `method` - default `POST`, [requestflow-spec.md §4](requestflow-spec.md#4-domain-model)).

| Field | Type | Always present? | Description |
| --- | --- | --- | --- |
| `contractVersion` | string | yes | See [§7](#7-contract-versioning). `"1.0"` for this draft. |
| `sessionId` | string | yes | Stable for the life of the conversation. |
| `turnIndex` | integer | yes | Monotonically increasing per session, starting at 0. Half of the idempotency key ([§5](#5-idempotency)). |
| `traceId` | string | yes | Correlates with VASDK's own frame tracing (`Frame::sourceId`/sequence) - include it verbatim in any logs. |
| `activeFlowId` | string | yes | The flow the session is on when this call fires. |
| `activeNodeId` | string | yes | The node. |
| `routeId` | string | yes | The specific route whose executor triggered this call. |
| `utterance` | string | no | The last user utterance, if this turn had one (absent for a trigger-driven turn, see [requestflow-spec.md §9.7](requestflow-spec.md#97-external-and-scheduled-triggers)). |
| `parameters` | object | yes | Current session parameters, flat key/value. Value types follow the originating `Parameter.Type` ([requestflow-spec.md §7](requestflow-spec.md#7-parameter-model-and-collection-policy)) - `STRING`/`ENUM` as JSON strings, `NUMBER`/`INTEGER` as JSON numbers, `BOOLEAN` as JSON booleans, `DATE`/`TIME`/`DATETIME` as ISO 8601 strings, `OBJECT`/`COMPOSITE` as nested JSON objects. May be `{}`. |
| `speakerId` | string | no | Present only if the session has one ([requestflow-spec.md §9.8](requestflow-spec.md#98-permission-gate)). |

```json
{
  "contractVersion": "1.0",
  "sessionId": "sess_8f3a1c",
  "turnIndex": 4,
  "traceId": "frame_10293",
  "activeFlowId": "audio",
  "activeNodeId": "start_playback",
  "routeId": "found",
  "utterance": "play some jazz",
  "parameters": { "genre": "jazz", "artist": null },
  "speakerId": "driver_1"
}
```

## 3. The Webhook Response

All fields optional - an empty `{}` is a valid, no-op response (the turn just continues with whatever the route's own executor does next).

| Field | Type | Description |
| --- | --- | --- |
| `messages` | array of string | Each becomes a `say`-equivalent `TextFrame`, in order, before anything else in the response is applied. |
| `parameterPatches` | object | Merged into session `parameters` - same value-typing rules as the request's `parameters`. A key not already declared on any `Parameter` in the active flow is stored as an untyped session value (this is exactly how `catalog_result_found`-style tool-computed flags work in the worked examples - they're never in a node's static `form`). |
| `transition` | object `{ targetType, targetId }` | Same shape as `Transition` ([requestflow-spec.md §4](requestflow-spec.md#4-domain-model)) - `targetType` one of `NODE`/`FLOW`/`END_SESSION`/`NONE`. A `targetId` that doesn't resolve against the live `Agent` is treated as an engine-side validation failure, surfacing as `ErrorFrame` upstream to `RecoveryProcessor` - **the backend cannot itself verify this is a real node/flow id**, since it has no visibility into the loaded config; get the id right, or don't set `transition` and let the route's own config-level `Transition` (if any) apply instead. |
| `emitEvents` | array of string | Each becomes an `EmitEventAction`-equivalent, applied as pending events for the next evaluation step ([requestflow-spec.md §6](requestflow-spec.md#6-route-evaluation-semantics)). Plain names only in this field - a webhook response cannot attach `event.*` params ([requestflow-spec.md §8.2](requestflow-spec.md#82-the-event-namespace)) the way a `customTool` handler can via `setParameter`; if a caught event needs data, put it in `parameterPatches` instead and read it from `parameters`, not `event.*`. |
| `handoff` | object `{ reason }` | Fires `setOnHandoff` the same as a `HandoffAction`. |
| `endSession` | boolean | `true` ends the session after this turn's response is applied. |

```json
{
  "messages": ["Now playing Kind of Blue by Miles Davis."],
  "parameterPatches": {
    "catalog_result_found": true,
    "now_playing_track": "Kind of Blue",
    "now_playing_artist": "Miles Davis",
    "now_playing_track_id": "trk_492"
  }
}
```

```json
{
  "parameterPatches": { "catalog_result_found": false }
}
```

## 4. HTTP-Level Contract

- **Method**: whatever `CallWebhookAction.method` configures (default `POST`). Body is always JSON regardless of method; set `Content-Type: application/json` on the response.
- **Status codes**: `2xx` - the body is parsed as the response schema above. `4xx` - treated as a permanent failure, **no retry**, immediately surfaces as the `webhook_error` event ([requestflow-spec.md §9.2](requestflow-spec.md#92-callwebhook-execution-model)); use this for "this request is malformed and will never succeed," not for transient trouble. `5xx`, connection refused/reset, and DNS failure - treated as transient, retried once. A request that times out (see below) is also treated as transient.
- **Timeout budget**: the backend must respond within `CallWebhookAction.timeout_ms` (per-endpoint, default 1500 ms) *and* the call must complete within the turn's overall `turnExternalCallBudget` (default 2000 ms, shared across every external call that turn). Target a p99 well under 1500 ms - there is no partial credit for "almost fast enough," a timeout is indistinguishable from a 5xx to the engine.
- **Response size**: keep it small. `messages`/`parameterPatches` become `TextFrame`s and session state on the hot conversational path - this is not a bulk data channel.

## 5. Idempotency

`sessionId + turnIndex` is the idempotency key for every `callWebhook` invocation from a given route, whether this is the first attempt, the one retry [§4](#4-http-level-contract) allows, or - if the engine's `PendingActionStore` is configured ([requestflow-spec.md §9.9](requestflow-spec.md#99-offline-resilient-callwebhook-store-and-forward)) - a replay that arrives much later after an extended offline period.

**The backend must treat a repeated call with the same `(sessionId, turnIndex, routeId)` as safe to short-circuit.** At minimum: don't double-apply a side effect (charge a card twice, send a duplicate notification). Ideally: cache the response you gave the first time and return it again verbatim, so a retried call produces the exact same `parameterPatches`/`transition` the engine already might have partially processed. A `(sessionId, turnIndex)` pair is never reused for a different logical request.

## 6. Authentication

Nothing in [requestflow-spec.md §9.2](requestflow-spec.md#92-callwebhook-execution-model) specifies how a backend verifies a request actually came from the `OrchestratorEngine` it's paired with, rather than anything else that can reach its URL. This section is that missing piece.

**Scheme: HMAC-SHA256 request signing**, deliberately simple rather than OAuth2/mTLS - consistent with how [§9.8's permission gate](requestflow-spec.md#98-permission-gate) elsewhere in this project favors a narrow, scoped mechanism over a heavier standard. TLS (the transport itself) is assumed regardless; this scheme authenticates the *sender*, not the channel.

- **Secret**: a shared secret provisioned per `Agent`, out of band from the textproto config (never checked into source control alongside the flow definitions). The engine's `HttpInterface` implementation attaches it; the backend holds the same value in its own secret store.
- **Headers**:
  - `X-RequestFlow-Timestamp`: Unix epoch seconds when the request was signed.
  - `X-RequestFlow-Signature`: `sha256=<hex>`, computed as `HMAC-SHA256(secret, timestamp + "." + rawRequestBody)`.
- **Verification**: recompute the HMAC over the received timestamp and raw body using the shared secret; compare against the header using a constant-time comparison. Reject (`401`) on mismatch. Reject requests whose timestamp is more than 5 minutes old or in the future, to bound replay of a captured request even over an otherwise-valid signature.
- **Why the raw body, not a parsed/re-serialized one**: JSON re-serialization can reorder keys or normalize whitespace, silently invalidating a signature computed over a byte-for-byte different (but semantically identical) payload. Always sign and verify against the exact bytes on the wire, before any JSON parsing happens.

## 7. Contract Versioning

`contractVersion` ([§2](#2-the-webhook-request)) lets a backend and the engine evolve independently. Rule: **adding a new optional field to the request or response never bumps the version** - an old backend simply doesn't read a new field it doesn't know about, and the engine treats a response missing a newer optional field as that field being absent/default. Removing a field, changing a field's meaning, or changing a required field's type requires a new major version (`"2.0"`), and a backend should reject (`400`) a `contractVersion` it doesn't understand rather than guess.

## 8. Language Implementation Guides

Every stack needs exactly three ingredients: parse the JSON body into the request shape, verify the HMAC signature, and respond within budget with the response shape. Nothing below is VASDK-specific machinery - it's ordinary web-framework code.

### 8.1 Java / Spring Boot

```java
public record WebhookRequest(
    String contractVersion, String sessionId, int turnIndex, String traceId,
    String activeFlowId, String activeNodeId, String routeId,
    String utterance, Map<String, Object> parameters, String speakerId) {}

public record WebhookResponse(
    List<String> messages, Map<String, Object> parameterPatches,
    Transition transition, List<String> emitEvents,
    Handoff handoff, Boolean endSession) {}

@Component
public class SignatureFilter extends OncePerRequestFilter {
    @Value("${requestflow.webhook-secret}") String secret;

    @Override
    protected void doFilterInternal(HttpServletRequest req, HttpServletResponse res, FilterChain chain)
            throws IOException, ServletException {
        String body = req.getReader().lines().collect(Collectors.joining("\n"));
        String timestamp = req.getHeader("X-RequestFlow-Timestamp");
        String expected = "sha256=" + hmacSha256Hex(secret, timestamp + "." + body);
        if (!MessageDigest.isEqual(
                expected.getBytes(StandardCharsets.UTF_8),
                req.getHeader("X-RequestFlow-Signature").getBytes(StandardCharsets.UTF_8))) {
            res.sendError(HttpStatus.UNAUTHORIZED.value());
            return;
        }
        chain.doFilter(new CachedBodyRequestWrapper(req, body), res);
    }
}

@RestController
public class AudioWebhookController {
    @PostMapping("/webhooks/search_catalog")
    public WebhookResponse searchCatalog(@RequestBody WebhookRequest req) {
        var result = catalogService.search((String) req.parameters().get("genre"));
        return new WebhookResponse(null, Map.of(
            "catalog_result_found", result.isPresent(),
            "now_playing_track", result.map(Track::title).orElse(null)),
            null, null, null, null);
    }
}
```

### 8.2 Python (FastAPI)

```python
from pydantic import BaseModel
from fastapi import FastAPI, Request, HTTPException
import hmac, hashlib, time

WEBHOOK_SECRET = os.environ["REQUESTFLOW_WEBHOOK_SECRET"]

class WebhookRequest(BaseModel):
    contract_version: str
    session_id: str
    turn_index: int
    active_flow_id: str
    active_node_id: str
    route_id: str
    utterance: str | None = None
    parameters: dict
    speaker_id: str | None = None

app = FastAPI()

@app.middleware("http")
async def verify_signature(request: Request, call_next):
    body = await request.body()
    timestamp = request.headers.get("X-RequestFlow-Timestamp", "")
    if abs(time.time() - float(timestamp)) > 300:
        raise HTTPException(401, "stale timestamp")
    expected = "sha256=" + hmac.new(
        WEBHOOK_SECRET.encode(), f"{timestamp}.".encode() + body, hashlib.sha256
    ).hexdigest()
    if not hmac.compare_digest(expected, request.headers.get("X-RequestFlow-Signature", "")):
        raise HTTPException(401, "bad signature")
    return await call_next(request)

@app.post("/webhooks/search_catalog")
def search_catalog(req: WebhookRequest) -> dict:
    result = catalog.search(req.parameters.get("genre"))
    return {"parameterPatches": {
        "catalog_result_found": result is not None,
        "now_playing_track": result.title if result else None,
    }}
```

### 8.3 Go

```go
type WebhookRequest struct {
    ContractVersion string                 `json:"contractVersion"`
    SessionID       string                 `json:"sessionId"`
    TurnIndex       int                    `json:"turnIndex"`
    ActiveFlowID    string                 `json:"activeFlowId"`
    ActiveNodeID    string                 `json:"activeNodeId"`
    RouteID         string                 `json:"routeId"`
    Utterance       string                 `json:"utterance,omitempty"`
    Parameters      map[string]interface{} `json:"parameters"`
    SpeakerID       string                 `json:"speakerId,omitempty"`
}

func verifySignature(secret string, next http.HandlerFunc) http.HandlerFunc {
    return func(w http.ResponseWriter, r *http.Request) {
        body, _ := io.ReadAll(r.Body)
        r.Body = io.NopCloser(bytes.NewReader(body))

        ts := r.Header.Get("X-RequestFlow-Timestamp")
        mac := hmac.New(sha256.New, []byte(secret))
        mac.Write([]byte(ts + "."))
        mac.Write(body)
        expected := "sha256=" + hex.EncodeToString(mac.Sum(nil))

        if !hmac.Equal([]byte(expected), []byte(r.Header.Get("X-RequestFlow-Signature"))) {
            http.Error(w, "bad signature", http.StatusUnauthorized)
            return
        }
        next(w, r)
    }
}

func searchCatalogHandler(w http.ResponseWriter, r *http.Request) {
    var req WebhookRequest
    json.NewDecoder(r.Body).Decode(&req)
    result := catalog.Search(req.Parameters["genre"])
    json.NewEncoder(w).Encode(map[string]any{
        "parameterPatches": map[string]any{
            "catalog_result_found": result != nil,
        },
    })
}
```

### 8.4 Any other stack

The same three ingredients apply everywhere: a JSON-capable HTTP server, a constant-time HMAC comparison in whatever crypto library the language ships, and a handler returning the [§3](#3-the-webhook-response) shape within [§4](#4-http-level-contract)'s budget. Node/Express, Rust/Axum, Ruby/Rails, and anything else follow the identical pattern - there's nothing in this contract that favors one ecosystem over another.

## 9. Conformance Testing

A backend developer should be able to validate their endpoint without standing up the full VASDK C++ engine. Recommended approach (tooling not yet built - tracked in [§10](#10-open-decisions)): a small fixture-driven conformance script that POSTs a canonical set of request payloads (a happy path, a not-found case, a malformed-`contractVersion` case, a replayed idempotency key) against a target URL with a known test secret, and asserts the response matches [§3](#3-the-webhook-response)'s shape and the HTTP-level rules in [§4](#4-http-level-contract). Until that exists, the JSON examples in [§2](#2-the-webhook-request)/[§3](#3-the-webhook-response) are the reference to test against by hand.

## 10. Open Decisions

- Whether [§2](#2-the-webhook-request)/[§3](#3-the-webhook-response) should be published as a formal OpenAPI document as the canonical machine-readable schema, rather than the Markdown tables here being the source of truth. **Proposed scope, if built**: a single `POST {webhookUrl}` operation (the URL itself is config-defined per `CallWebhookAction`, not fixed by this contract, so this describes a request/response *body* schema shared by every endpoint, not a per-endpoint path catalog) - the [§2](#2-the-webhook-request) table as the request body schema, the [§3](#3-the-webhook-response) table as the response body schema, the [§4](#4-http-level-contract) status-code rules as documented `2xx`/`4xx`/`5xx` responses, and the [§6](#6-authentication) signature headers as a reusable `securityScheme`. Generated once per `contractVersion` ([§7](#7-contract-versioning)) rather than once overall, so an old and a new contract version can be published and diffed side by side - this is also what would let a future config-driven stub generator (the next bullet) target something machine-readable instead of re-deriving the shape from this Markdown by hand.
- The exact scope of a config-driven code generator that scaffolds backend stubs (Spring controller, FastAPI route, Go handler) per `CallWebhookAction` in an `Agent` config, targeting this contract - referenced in [§1](#1-scope) as related but not designed here.
- Whether the conformance-testing tool in [§9](#9-conformance-testing) should ship as a VASDK CLI subcommand or a standalone script/package. **Proposed fixture scope, if built**: at minimum one request/expectation pair per case already named in [§9](#9-conformance-testing) - a happy path, a not-found/empty-`parameterPatches` response, a malformed `contractVersion` (expect `400` per [§7](#7-contract-versioning)), and a replayed `(sessionId, turnIndex, routeId)` expecting the identical cached response per [§5](#5-idempotency) - each fixture carrying its request body, a test HMAC secret distinct from any real deployment's, and an assertion against the expected status code plus [§3](#3-the-webhook-response)'s response shape. Such a fixture set would also become this document's authoritative, executable version of the [§2](#2-the-webhook-request)/[§3](#3-the-webhook-response) JSON examples, superseding "test against these examples by hand."
- Whether mTLS should be documented as an additional, optional authentication mode for deployments that want it layered on top of (not instead of) the HMAC scheme in [§6](#6-authentication).
- Exact HMAC timestamp tolerance window (this draft proposes 5 minutes; may need to be configurable per deployment).
- **New (deferred - integrator tooling)**: secret scoping for a backend that fields webhooks for more than one `Agent`, or more than one deployment/environment of the same agent, behind a single service. [§6](#6-authentication) specifies "a shared secret provisioned per `Agent`" but not how such a backend looks up the *right* secret for a given inbound request (by the request's `activeFlowId`'s owning agent, a dedicated header, or per-agent URL path/subdomain), nor how a secret rotates without a window where both the old and new value must verify successfully.
