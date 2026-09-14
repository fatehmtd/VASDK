#pragma once

#include "vasdk_export.h"

#include <orchestrator/domain.pb.h>

#include <google/protobuf/struct.pb.h>
#include <optional>
#include <string>
#include <unordered_map>

namespace va {
namespace orchestrator {

// Milestone 1.6 (docs/reference/requestflow-roadmap.md §3) - the live,
// in-process counterpart to the persisted SessionSnapshot proto (spec §4/§10),
// per spec §13's proposed API. OrchestratorEngine owns the only instance;
// later milestones (a customTool handler, setOnFlowComplete) see it by
// reference, never SessionSnapshot directly - see spec §9.6.
//
// Spec §13's sketch is written from a customTool handler's-eye view: read a
// parameter, write a parameter, done. It doesn't show how the engine itself
// is supposed to move the session (active flow/node, turn count) as routes
// fire and transitions apply - those mutators are added here as the
// necessary engine-internal surface, not part of the spec's own read/write
// contract for handler code.
//
// Milestone 2.5 (spec §9.5) added ParameterMeta bookkeeping (attempts/source/
// last_updated_unix_ms - redact_in_logs still has nothing to redact from, no
// log line prints parameter values yet) and PendingAsk tracking, both driven
// by OrchestratorEngine's slot-filling logic (a static Node.form parameter or
// an ExpectAction's ephemeral one): noteParameterAskStarted/
// incrementParameterAttempts back a form parameter's reprompt/escalation
// cycle (ParameterMeta.attempts, keyed by parameter name, cleared alongside
// the value by clearParameter/clearAllParameters - Milestone 3.4 confirmed
// this via a cross-flow preserveParameters=false test - since a cleared
// parameter's next ask should start over); the 3-arg setParameter overload
// additionally records
// source/last_updated_unix_ms on a successful slot fill (attempts
// deliberately untouched - it's a historical failed-attempt count, not reset
// by success). PendingAsk is a single slot, not a map, since spec §9.5 is
// explicit that only one `expect` can be outstanding at a time; hasPendingAsk()
// is tracked as its own bool because a bare turn-wait's PendingAsk (no
// `parameter` set, attempts 0, transient false) is otherwise
// indistinguishable from "no ask armed at all" by content alone.
// OrchestratorEngine::snapshot()/the restoring constructor round-trip both of
// these, since spec §9.5 explicitly requires a mid-ask session to survive
// restore.
class VASDK_EXPORT SessionState {
public:
    SessionState(std::string sessionId, std::string activeFlowId, std::string activeNodeId);

    // M1.7 (session persistence, spec §10) - restores a session's identity
    // fields with a caller-supplied creation timestamp instead of "now",
    // since a restored session's sessionElapsedMs (buildActivation()) must
    // keep counting from the original session's start, not reset at restore
    // time.
    SessionState(std::string sessionId, std::string activeFlowId, std::string activeNodeId,
                 int64_t createdAtUnixMs);

    const std::string& sessionId() const;
    const std::string& activeFlowId() const;
    const std::string& activeNodeId() const;
    int turnCount() const;
    int64_t createdAtUnixMs() const;

    std::optional<google::protobuf::Value> getParameter(const std::string& name) const;
    void setParameter(const std::string& name, google::protobuf::Value value);

    // Milestone 2.5 (spec §9.5/§7) - same as the 2-arg overload, plus records
    // `source` ("nlu" for a static-form parameter filled via NLUProvider
    // extraction, "expect" for an ExpectAction's ephemeral parameter) and the
    // current time into parameterMetaAll()'s entry for `name` (creating one if
    // noteParameterAskStarted/incrementParameterAttempts hadn't already).
    void setParameter(const std::string& name, google::protobuf::Value value, std::string source);

    void clearParameter(const std::string& name);

    // Read-only convenience view, e.g. for logging or building a CEL Activation.
    const google::protobuf::Struct& parameters() const;

    // Engine-internal mutators - see the class comment above for why these
    // exist beyond spec §13's handler-facing sketch.
    void setActiveFlowId(std::string flowId);
    void setActiveNodeId(std::string nodeId);
    void incrementTurnCount();

    // Transition.preserve_parameters == false (spec §4) - drops every session
    // parameter on a cross-flow transition that opted out of carrying them.
    void clearAllParameters();

    // Milestone 2.5 (spec §9.5/§7) - a static Node.form parameter's
    // reprompt/escalation cycle. noteParameterAskStarted is idempotent
    // (creates a fresh {attempts:0} entry only the first time a given
    // parameter name is noticed as required-and-unfilled); incrementParameterAttempts
    // creates one too if needed and returns the post-increment count (1-based -
    // the count of failed attempts so far this ask). Both are no-ops/queries
    // against parameterMetaAll(), never against PendingAsk - a typed `expect`
    // tracks its own attempts on PendingAsk.attempts instead (see below), since
    // that value has to travel with the snapshot as part of pending_ask, not
    // keyed by a name that isn't in any node's static form.
    void noteParameterAskStarted(const std::string& name);
    int incrementParameterAttempts(const std::string& name);
    int parameterAttempts(const std::string& name) const;

    // Restore-only direct setter (OrchestratorEngine's restoring constructor) -
    // live bookkeeping goes through the methods above instead.
    void setParameterMeta(const std::string& name, ParameterMeta meta);
    const std::unordered_map<std::string, ParameterMeta>& parameterMetaAll() const;

    // Milestone 2.5 (spec §9.5) - at most one ExpectAction can be outstanding
    // at a time; see the class comment above for why hasPendingAsk() is a
    // separate bool rather than inferred from PendingAsk's own contents.
    bool hasPendingAsk() const;
    const PendingAsk& pendingAsk() const;
    void setPendingAsk(PendingAsk ask);
    void clearPendingAsk();
    int incrementPendingAskAttempts();

private:
    std::string _sessionId;
    std::string _activeFlowId;
    std::string _activeNodeId;
    int _turnCount = 0;
    int64_t _createdAtUnixMs;
    google::protobuf::Struct _parameters;
    std::unordered_map<std::string, ParameterMeta> _parameterMeta;
    PendingAsk _pendingAsk;
    bool _hasPendingAsk = false;
};

} // namespace orchestrator
} // namespace va
