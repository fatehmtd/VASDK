#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalState.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <VASDK/metrics/CallMetrics.h>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace va {
namespace goal {

class VASDK_EXPORT GoalManager {
public:
    /// maxReprompts - after this many failed collection attempts the LLM is
    /// instructed to escalate rather than keep asking.
    explicit GoalManager(GoalDefinition def, int maxReprompts = 2);

    //  Pre-call slot pre-fill 

    /// Inject a slot value before the pipeline starts (session data, external lookup, etc.).
    void prefillSlot(const std::string& dotPath, const nlohmann::json& value);

    //  Wiring 

    /// Registers {goal_objective}, {collected_slots}, {missing_slots}, {goal_status},
    /// and {escalation_hint} template variable resolvers on the transport.
    /// App must set a PromptTemplate using these variables before calling this,
    /// and call this before registering the {tool_descriptions} resolver (also app-provided).
    void attachToLLMTransport(va::llm::LLMTransport* transport);

    /// Registers the update_slot, update_object_slot, request_confirmation,
    /// complete_goal, and escalate tools.
    void attachToToolDispatcher(va::llm::ToolDispatchProcessor* dispatcher);

    /// Convenience: calls attachToLLMTransport then attachToToolDispatcher.
    void attach(va::llm::LLMTransport* transport, va::llm::ToolDispatchProcessor* dispatcher);

    //  Callbacks 

    struct EscalationContext {
        std::string    reason;
        nlohmann::json collectedSlots; ///< All slots collected so far at the time of escalation
    };

    /// Called after complete_goal fires the external action successfully.
    /// Typically stops the pipeline: goalManager->setOnComplete([&]{ pipeline->stop(); });
    void setOnComplete(std::function<void()> fn);

    /// Called when the escalate tool fires. App should route to the appropriate
    /// handler - human operator, supervisor system, fallback service, etc.
    void setOnEscalate(std::function<void(const EscalationContext&)> fn);

    //  State access 

    const GoalState& state() const;
    bool isComplete() const;

    /// Replace the active goal definition and reset all slot state.
    /// Used by Phase 4 (goal branching / mid-call goal switch).
    void reset(GoalDefinition newGoal);

    /// Attach a CallMetrics instance for per-slot tracking. Optional.
    void setMetrics(std::weak_ptr<va::metrics::CallMetrics> metrics);

private:
    GoalState              _state;
    std::function<void()>  _onComplete;
    std::function<void(const EscalationContext&)> _onEscalate;
    int                    _maxReprompts;
    va::llm::ToolDispatchProcessor* _dispatcher = nullptr;
    std::weak_ptr<va::metrics::CallMetrics>       _metrics;

    va::llm::Tool makeUpdateSlotTool();
    va::llm::Tool makeUpdateObjectSlotTool();
    va::llm::Tool makeRequestConfirmationTool();
    va::llm::Tool makeCompleteGoalTool();
    va::llm::Tool makeEscalateTool();
};

} // namespace goal
} // namespace va
