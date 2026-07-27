#include <VASDK/goal/ConditionEvaluator.h>
#include <VASDK/goal/GoalState.h>

#include <algorithm>
#include <stdexcept>

namespace va {
namespace goal {

// Forward-declare so recursive compilation works
static std::function<bool(const GoalState&)> compile(const nlohmann::json& expr);

static std::function<bool(const GoalState&)> compile(const nlohmann::json& expr) {
    if (!expr.is_object())
        throw std::invalid_argument("[ConditionEvaluator] condition must be a JSON object");

    std::string op = expr.value("op", "");

    //  Logical combinators 

    if (op == "and") {
        std::vector<std::function<bool(const GoalState&)>> children;
        for (const auto& c : expr.at("conditions")) children.push_back(compile(c));
        return [children](const GoalState& s) {
            for (const auto& fn : children) if (!fn(s)) return false;
            return true;
        };
    }

    if (op == "or") {
        std::vector<std::function<bool(const GoalState&)>> children;
        for (const auto& c : expr.at("conditions")) children.push_back(compile(c));
        return [children](const GoalState& s) {
            for (const auto& fn : children) if (fn(s)) return true;
            return false;
        };
    }

    if (op == "not") {
        auto child = compile(expr.at("condition"));
        return [child](const GoalState& s) { return !child(s); };
    }

    //  Slot-based predicates 

    std::string slotName = expr.value("slot", "");
    if (slotName.empty())
        throw std::invalid_argument("[ConditionEvaluator] 'slot' is required for op: " + op);

    if (op == "exists") {
        return [slotName](const GoalState& s) {
            return s.slotValueJson(slotName) != nullptr;
        };
    }

    if (op == "eq") {
        std::string expected = expr["value"].get<std::string>();
        return [slotName, expected](const GoalState& s) {
            return s.slotValueStr(slotName) == expected;
        };
    }

    if (op == "neq") {
        std::string expected = expr["value"].get<std::string>();
        return [slotName, expected](const GoalState& s) {
            return s.slotValueStr(slotName) != expected;
        };
    }

    if (op == "in") {
        std::vector<std::string> values;
        for (const auto& v : expr.at("value")) values.push_back(v.get<std::string>());
        return [slotName, values](const GoalState& s) {
            auto val = s.slotValueStr(slotName);
            return std::find(values.begin(), values.end(), val) != values.end();
        };
    }

    if (op == "nin") {
        std::vector<std::string> values;
        for (const auto& v : expr.at("value")) values.push_back(v.get<std::string>());
        return [slotName, values](const GoalState& s) {
            auto val = s.slotValueStr(slotName);
            return std::find(values.begin(), values.end(), val) == values.end();
        };
    }

    if (op == "gte") {
        double threshold = expr["value"].get<double>();
        return [slotName, threshold](const GoalState& s) {
            auto j = s.slotValueJson(slotName);
            return j.is_number() && j.get<double>() >= threshold;
        };
    }

    if (op == "lte") {
        double threshold = expr["value"].get<double>();
        return [slotName, threshold](const GoalState& s) {
            auto j = s.slotValueJson(slotName);
            return j.is_number() && j.get<double>() <= threshold;
        };
    }

    throw std::invalid_argument("[ConditionEvaluator] Unknown operator: " + op);
}

std::function<bool(const GoalState&)> compileCondition(const nlohmann::json& expr) {
    return compile(expr);
}

} // namespace goal
} // namespace va
