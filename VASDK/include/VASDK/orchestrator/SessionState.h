#pragma once

#include "vasdk_export.h"

#include <google/protobuf/struct.pb.h>
#include <optional>
#include <string>

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
// Not yet tracked: ParameterMeta bookkeeping (attempts/source/last_updated)
// and redact_in_logs - nothing in M1.6 has a "source" to record (no NLU, no
// webhook patches, no expect) or a log line to redact from, so there's
// nothing to populate yet. M1.7 (session persistence) is what will need
// parameter_meta populated; until then OrchestratorEngine::snapshot() just
// leaves it empty.
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

private:
    std::string _sessionId;
    std::string _activeFlowId;
    std::string _activeNodeId;
    int _turnCount = 0;
    int64_t _createdAtUnixMs;
    google::protobuf::Struct _parameters;
};

} // namespace orchestrator
} // namespace va
