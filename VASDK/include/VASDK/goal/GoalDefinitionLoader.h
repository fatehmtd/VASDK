#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalDefinition.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace va {
namespace goal {

/// Deserialises GoalDefinition objects from JSON.
///
/// Condition lambdas are compiled by ConditionEvaluator.
/// The onComplete callback and any custom slot validators cannot be serialised -
/// they must be registered by the app after loading:
///
///   auto goal = GoalDefinitionLoader::fromFile("goals/schedule.json");
///   goal.onComplete = myHandlers["schedule_callback"];
class VASDK_EXPORT GoalDefinitionLoader {
public:
    static GoalDefinition              fromJson(const nlohmann::json& j);
    static GoalDefinition              fromFile(const std::string& path);
    static std::vector<GoalDefinition> fromDirectory(const std::string& dir);
};

} // namespace goal
} // namespace va
