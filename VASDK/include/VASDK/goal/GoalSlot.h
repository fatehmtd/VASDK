#pragma once

#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace goal {

class GoalState; // forward declaration - used in condition predicate

//  Slot type 

enum class SlotType {
    STRING,    ///< Free text; passed through as-is
    PHONE,     ///< Normalised to NXX-NXX-XXXX (US) or NXX-XXXX (local)
    EMAIL,     ///< Validated for basic format
    DATE,      ///< ISO 8601 YYYY-MM-DD; LLM resolves natural language before calling
    TIME,      ///< HH:MM (24-hour)
    DATETIME,  ///< ISO 8601 combined datetime
    NUMBER,    ///< Floating-point; optional min/max
    INTEGER,   ///< Integer; optional min/max
    BOOLEAN,   ///< Accepts yes/no/true/false/1/0; stored as json bool
    ENUM,      ///< Must match one of allowedValues (case-insensitive)
    OBJECT,    ///< json object; validated against objectSchema
    COMPOSITE, ///< Named subSlots filled individually via dot-path notation
};

//  Object field schema 

struct ObjectField {
    std::string name;
    std::string description;
    std::string jsonType = "string"; ///< "string", "number", "integer", "boolean"
    bool        required = true;
};

//  Constraints 

struct SlotConstraints {
    std::vector<std::string> allowedValues; ///< ENUM - shown in LLM prompt
    double min = -std::numeric_limits<double>::infinity();
    double max =  std::numeric_limits<double>::infinity();
    bool mustBeFuture = false;              ///< DATE / DATETIME
    std::vector<ObjectField> objectSchema;  ///< OBJECT - expected fields and types
};

//  Validation result 

struct SlotValidationResult {
    bool           valid;
    nlohmann::json normalizedValue; ///< Canonical form - json string for scalars, object for OBJECT
    std::string    errorMessage;    ///< Returned to LLM on failure so it can re-prompt
};

//  Slot definition 

struct GoalSlot {
    std::string name;
    std::string description; ///< Shown in LLM system prompt

    SlotType type     = SlotType::STRING;
    bool     required = true;

    SlotConstraints    constraints;
    std::vector<GoalSlot> subSlots; ///< COMPOSITE only

    /// Conditional requirement. nullptr = always apply `required`.
    /// Lambda receives the live GoalState so conditions can reference other slots.
    std::function<bool(const GoalState&)> condition;

    /// Custom validator - receives parsed nlohmann::json, overrides built-in validation.
    std::function<SlotValidationResult(const nlohmann::json&)> validator;

    /// Runtime value - nlohmann::json regardless of SlotType:
    ///   STRING/PHONE/DATE/ENUM/… → json string
    ///   NUMBER/INTEGER            → json number
    ///   BOOLEAN                   → json bool
    ///   OBJECT                    → json object
    ///   COMPOSITE                 → unused; subSlots hold individual values
    std::optional<nlohmann::json> value;
};

} // namespace goal
} // namespace va
