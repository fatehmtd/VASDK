#pragma once

// For C++ code that works alongside C code using the C API (vasdk.h): what its handles hold.
// ProviderCatalog's vasdk_providers_add_all uses it, and so can an app with providers of its own.

#include "vasdk.h"
#include <VASDK/vasdk_export.h>
#include "../engine/ProviderRegistry.h"
#include "../flow/CompiledAgent.h"

#include <memory>

/** The providers a vasdk_providers holds: add to it, as engine::ProviderRegistry::add. */
VASDK_EXPORT va::engine::ProviderRegistry& vasdk_providers_registry(vasdk_providers* providers);
/** The compiled agent a vasdk_agent holds. */
VASDK_EXPORT std::shared_ptr<const va::flow::CompiledAgent> vasdk_agent_compiled(const vasdk_agent* agent);
