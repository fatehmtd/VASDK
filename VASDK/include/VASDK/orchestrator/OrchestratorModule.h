#pragma once

#include "vasdk_export.h"

namespace va {
namespace orchestrator {

// Milestone 0 placeholder - proves the va::orchestrator target, include path, and
// namespace compile and link cleanly. Domain types (Agent/Flow/Node/Route/...)
// arrive in Milestone 1.1 as a generated .proto schema - see
// docs/reference/requestflow-spec.md §4 and docs/reference/requestflow-roadmap.md.
VASDK_EXPORT const char* moduleVersion();

} // namespace orchestrator
} // namespace va
