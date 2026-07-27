#include <VASDK/goal/GoalConversationSimulator.h>
#include <VASDK/llm/LLMTransport.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace va {
namespace goal {

GoalConversationSimulator::GoalConversationSimulator(GoalDefinition def)
    : _def(std::move(def)) {}

GoalConversationSimulator& GoalConversationSimulator::addTurn(SimTurn turn) {
    _turns.push_back(std::move(turn));
    return *this;
}

SimulationResult GoalConversationSimulator::run() {
    _ran      = false;
    _escalated = false;
    _result   = {};

    // Build a standalone GoalManager + ToolDispatchProcessor (no real pipeline needed)
    _dispatcher  = std::make_shared<va::llm::ToolDispatchProcessor>();
    _goalManager = std::make_shared<GoalManager>(_def);

    bool completed = false;
    _goalManager->setOnComplete([&]{ completed = true; });
    _goalManager->setOnEscalate([&](const GoalManager::EscalationContext&) {
        _escalated = true;
    });

    // Attach tools only (no real LLM transport needed for simulation)
    _goalManager->attachToToolDispatcher(_dispatcher.get());

    for (size_t i = 0; i < _turns.size(); ++i) {
        spdlog::debug("[Simulator] Turn {}/{}: \"{}\"", i + 1, _turns.size(), _turns[i].userText);
        executeTurn(_turns[i]);
    }

    _result.goalCompleted = completed || _goalManager->isComplete();
    _result.escalated     = _escalated;
    _result.finalSlots    = _goalManager->state().toJson();
    _result.success       = true;
    _ran = true;
    return _result;
}

void GoalConversationSimulator::executeTurn(const SimTurn& turn) {
    // Drive each scripted tool call through ToolDispatchProcessor
    for (const auto& call : turn.llmToolCalls) {
        auto frame      = std::make_shared<va::transport::ToolCallFrame>();
        frame->toolName = call.toolName;
        frame->argsJson = call.args.dump();

        spdlog::debug("[Simulator]   -> tool: {} args: {}", call.toolName, frame->argsJson);

        _dispatcher->handleToolCall(frame);
    }
}

//  Fluent assertions 

GoalConversationSimulator& GoalConversationSimulator::assertSlotValue(
    const std::string& name, const nlohmann::json& expected) {
    if (!_ran) throw std::logic_error("Call run() before asserting");
    auto actual = _goalManager->state().slotValueJson(name);
    if (actual != expected) {
        throw std::runtime_error(
            "[Simulator] assertSlotValue failed for '" + name + "': "
            "expected " + expected.dump() + " got " + actual.dump());
    }
    return *this;
}

GoalConversationSimulator& GoalConversationSimulator::assertGoalComplete() {
    if (!_ran) throw std::logic_error("Call run() before asserting");
    if (!_result.goalCompleted) {
        throw std::runtime_error(
            "[Simulator] assertGoalComplete failed - goal not complete. "
            "Still needed: " + _goalManager->state().missingSlotsSummary());
    }
    return *this;
}

GoalConversationSimulator& GoalConversationSimulator::assertEscalated() {
    if (!_ran) throw std::logic_error("Call run() before asserting");
    if (!_result.escalated) {
        throw std::runtime_error("[Simulator] assertEscalated failed - escalation was not triggered");
    }
    return *this;
}

GoalConversationSimulator& GoalConversationSimulator::assertSlotMissing(const std::string& name) {
    if (!_ran) throw std::logic_error("Call run() before asserting");
    auto val = _goalManager->state().slotValueJson(name);
    if (!val.is_null()) {
        throw std::runtime_error(
            "[Simulator] assertSlotMissing failed for '" + name
            + "' - slot has value: " + val.dump());
    }
    return *this;
}

} // namespace goal
} // namespace va
