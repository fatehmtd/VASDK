// vasdk_agentc for the garage agent: it knows the agent's C++ steps and envelopes.
//   garage_agentc test agents/garage-menu/agent.txtpb agents/garage-menu/tests/*.txtpb
//   garage_agentc chat agents/garage-menu/agent.txtpb
//   garage_agentc eval agents/garage-menu/agent.txtpb agents/garage-menu/nlu_eval.txtpb --engine-config <engine.txtpb>

#include "GarageExtensions.h"

#include <VASDK/flow/AgentTool.h>
#ifdef VASDK_HAS_PROVIDER_CATALOG
#include "ProviderCatalog/ProviderCatalog.h"
#endif

int main(int argc, char** argv) {
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    garage::registerExtensions(*registry);
#ifdef VASDK_HAS_PROVIDER_CATALOG
    va::engine::ProviderRegistry providers;   // the sentence encoder and entity model --engine-config names
    va::thirdparty::registerAllProviders(providers);
    return va::flow::runAgentTool(argc, argv, registry, &providers);
#else
    return va::flow::runAgentTool(argc, argv, registry);
#endif
}
