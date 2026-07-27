#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalSlot.h>
#include <functional>
#include <nlohmann/json.hpp>

namespace va {
namespace goal {

/// Compiles a JSON predicate expression into a GoalSlot condition lambda.
///
/// Supported operators:
///   {"slot":"name", "op":"eq",  "value":"repair"}
///   {"slot":"name", "op":"neq", "value":"..."}
///   {"slot":"name", "op":"in",  "value":["a","b"]}
///   {"slot":"name", "op":"nin", "value":["a","b"]}
///   {"slot":"name", "op":"exists"}           // slot has any value
///   {"slot":"name", "op":"gte", "value":2}
///   {"slot":"name", "op":"lte", "value":5}
///   {"op":"and", "conditions":[...]}
///   {"op":"or",  "conditions":[...]}
///   {"op":"not", "condition":{...}}
///
/// Throws std::invalid_argument on unknown operator or malformed expression.
VASDK_EXPORT std::function<bool(const GoalState&)>
compileCondition(const nlohmann::json& expr);

} // namespace goal
} // namespace va
