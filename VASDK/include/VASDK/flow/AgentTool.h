#pragma once

#include "vasdk_export.h"
#include "FlowRegistry.h"

#include <memory>

namespace va {
namespace flow {

/**
 * @brief vasdk_agentc's commands (lint, test, chat, try), for an app's own copy of it that knows
 *        the C++ steps, page types and envelopes its agents use.
 *
 * @code
 * int main(int argc, char** argv) {
 *     auto registry = std::make_shared<va::flow::FlowRegistry>();
 *     acme::registerExtensions(*registry);
 *     return va::flow::runAgentTool(argc, argv, registry);   // acme_agentc test agent.txtpb tests/*.txtpb
 * }
 * @endcode
 */
VASDK_EXPORT int runAgentTool(int argc, char** argv, std::shared_ptr<const FlowRegistry> registry = nullptr);

} // namespace flow
} // namespace va
