// vasdk_agentc for the garage agent: it knows the agent's C++ steps and envelopes.
//   garage_agentc test agents/garage-menu/agent.txtpb agents/garage-menu/tests/*.txtpb
//   garage_agentc chat agents/garage-menu/agent.txtpb

#include "GarageExtensions.h"

#include <VASDK/flow/AgentTool.h>

int main(int argc, char** argv) {
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    garage::registerExtensions(*registry);
    return va::flow::runAgentTool(argc, argv, registry);
}
