#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalDefinition.h>
#include <VASDK/goal/GoalManager.h>
#include <VASDK/goal/IntentRouter.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace goal {

/// A single scripted tool call that the simulated LLM will emit.
struct SimToolCall {
    std::string    toolName;
    nlohmann::json args;
};

/// A single conversation turn: the user's utterance plus the tool calls
/// the simulated LLM makes in response.
struct SimTurn {
    std::string                  userText;     ///< Injected as if transcribed from the user
    std::vector<SimToolCall>     llmToolCalls; ///< Tool calls emitted by the scripted LLM
    std::optional<std::string>   llmResponse;  ///< Optional spoken response (informational only)
};

struct SimulationResult {
    bool        success = false;
    std::string failureReason;
    nlohmann::json finalSlots;   ///< GoalState::toJson() at end of simulation
    bool        goalCompleted  = false;
    bool        escalated      = false;
};

/// Simulates multi-turn goal-oriented conversations without a real LLM or audio device.
///
/// Replaces LLMProcessor with a scripted driver that emits ToolCallFrames in the
/// prescribed order.  Uses MockTransport for audio I/O.  Pure logic testing.
///
/// Usage:
///   GoalConversationSimulator sim(goal);
///   sim.addTurn({.userText="Alice Johnson", .llmToolCalls={
///       {"update_slot", {{"slot_name","customer_name"},{"value","Alice Johnson"}}}
///   }});
///   auto result = sim.run();
///   assert(result.goalCompleted);
class VASDK_EXPORT GoalConversationSimulator {
public:
    explicit GoalConversationSimulator(GoalDefinition def);

    GoalConversationSimulator& addTurn(SimTurn turn);

    /// Execute all scripted turns synchronously and return the final state.
    SimulationResult run();

    //  Fluent assertions (call after run()) 
    GoalConversationSimulator& assertSlotValue(const std::string& name,
                                               const nlohmann::json& expected);
    GoalConversationSimulator& assertGoalComplete();
    GoalConversationSimulator& assertEscalated();
    GoalConversationSimulator& assertSlotMissing(const std::string& name);

private:
    GoalDefinition            _def;
    std::vector<SimTurn>      _turns;
    SimulationResult          _result;
    bool                      _ran     = false;
    bool                      _escalated = false;

    std::shared_ptr<GoalManager>          _goalManager;
    std::shared_ptr<va::llm::ToolDispatchProcessor> _dispatcher;

    void executeTurn(const SimTurn& turn);
};

} // namespace goal
} // namespace va
