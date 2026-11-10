#pragma once

#include <VASDK/vasdk_export.h>
#include "FlowRegistry.h"

#include <memory>

namespace va {
namespace engine { class ProviderRegistry; }
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
 *
 * `providers` (thirdparty::registerAllProviders fills one) lets `--engine-config` load its
 * sentence_encoder and entity_model (minilm, gliner); without it only the llm and tagger ones that
 * need no provider work... and tagger needs a sentence encoder.
 */
VASDK_EXPORT int runAgentTool(int argc, char** argv, std::shared_ptr<const FlowRegistry> registry = nullptr,
                              const engine::ProviderRegistry* providers = nullptr);

} // namespace flow
} // namespace va
