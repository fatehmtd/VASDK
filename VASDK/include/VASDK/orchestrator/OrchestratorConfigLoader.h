#pragma once

#include "vasdk_export.h"
#include <orchestrator/domain.pb.h>
#include <string>
#include <vector>

namespace va {
namespace orchestrator {

/// Offline semantic-validation result for an Agent config - referential
/// integrity, reachability, handler coverage, and the spec §9.5 dead-end-node
/// check. Never serialised - an in-process return value only, same posture as
/// goal/'s SlotValidationResult.
///
/// `errors` are hard failures (ok becomes false): a bad transition target_id,
/// a duplicate id, or a §9.5 dead-end node. `warnings` are best-effort,
/// non-blocking findings - reachability in particular is inherently
/// conservative (a node can be legitimately reached only via an ambient
/// trigger, §9.7, or a webhook response's dynamic transition, neither of
/// which is visible to a static graph walk over the config alone).
struct VASDK_EXPORT ValidationReport {
    bool ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Loads and offline-validates an Agent from a .proto-schema'd config file
/// (or a directory of them) - see requestflow-spec.md §13/§14/§14.1.
class VASDK_EXPORT OrchestratorConfigLoader {
public:
    /// One self-contained file, human-authored, comments allowed.
    static va::orchestrator::Agent fromTextprotoFile(const std::string& path);

    /// Compact binary form, for generated/exported configs.
    static va::orchestrator::Agent fromBinaryFile(const std::string& path);

    /// Composes agent.textproto + flows/*.textproto - see spec §14.1.
    /// Flow files are parsed in lexical filename order and appended to
    /// Agent.flows; a flow id collision (against agent.textproto or another
    /// flow file) throws rather than silently shadowing.
    static va::orchestrator::Agent fromDirectory(const std::string& dirPath);

    /// Referential integrity, reachability, handler coverage, and the §9.5
    /// dead-end-node check - offline, no CEL compilation yet (that needs
    /// cel-cpp, deferred - see requestflow-roadmap.md).
    static ValidationReport validate(const va::orchestrator::Agent& agent);
};

} // namespace orchestrator
} // namespace va
