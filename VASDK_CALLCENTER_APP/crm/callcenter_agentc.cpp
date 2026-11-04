// vasdk_agentc for the call center agent: it knows the agent's CRM tools.
//   callcenter_agentc test agents/callcenter/agent.txtpb agents/callcenter/tests/*.txtpb
//   callcenter_agentc chat agents/callcenter/agent.txtpb --session caller=555-1234
//   callcenter_agentc test ... --llm     (the same suite with an LLM after the rules)

#include "CallCenterTools.h"

#include <VASDK/flow/AgentTool.h>

int main(int argc, char** argv) {
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    callcenter::registerTools(*registry);
    return va::flow::runAgentTool(argc, argv, registry);
}
