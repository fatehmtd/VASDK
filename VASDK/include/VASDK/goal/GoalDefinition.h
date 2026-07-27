#pragma once

#include <VASDK/goal/GoalSlot.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace va {
namespace goal {

struct GoalDefinition {
    std::string name;
    std::string objective; ///< Shown to LLM as the task description

    std::vector<GoalSlot> slots;

    /// Called when all required slots are confirmed and complete.
    /// Receives all collected slot values as a json object.
    /// Returns true on success, false to signal the external action failed.
    std::function<bool(const nlohmann::json&)> onComplete;
};

} // namespace goal
} // namespace va
