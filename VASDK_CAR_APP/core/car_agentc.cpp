// vasdk_agentc for the car agent: it knows every vertical's tools and entity extractors.
//   car_agentc chat agents/car/agent.txtpb
//   car_agentc test agents/car/agent.txtpb verticals/*/tests/*.txtpb
//   car_agentc try agents/car/agent.txtpb "take me to the airport"

#include "Car.h"

#include <VASDK/flow/AgentTool.h>

int main(int argc, char** argv) {
    auto car = car::makeCar();
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    car->registerWith(*registry);
    return va::flow::runAgentTool(argc, argv, registry);
}
