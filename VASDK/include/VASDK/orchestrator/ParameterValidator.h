#pragma once

#include "vasdk_export.h"
#include <VASDK/orchestrator/ExpressionEvaluator.h>
#include <orchestrator/domain.pb.h>

#include <functional>
#include <google/protobuf/struct.pb.h>
#include <map>
#include <string>

namespace va {
namespace orchestrator {

// Milestone 1.5 (docs/reference/requestflow-roadmap.md §3) - a straight port
// of goal/'s GoalSlot validation/normalization semantics (VASDK/src/goal/
// GoalState.cpp) onto the Parameter proto (spec §7). Same rules, same
// escape hatches, renamed to fit this module; see the per-type table in
// spec §7. Never serialised - an in-process return value only, same posture
// as goal/'s SlotValidationResult and this module's own ValidationReport.
struct VASDK_EXPORT ParameterValidationResult {
    bool valid = false;
    google::protobuf::Value normalizedValue;   // NUMBER and INTEGER both
                                                // normalize to a NUMBER_VALUE
                                                // double - google::protobuf::
                                                // Value has no separate
                                                // integer kind, matching the
                                                // convention M1.3/M1.4 already
                                                // use for captured numbers.
    std::string errorMessage;                  // user-facing; drives the reprompt (spec §7)
};

class VASDK_EXPORT ParameterValidator {
public:
    // A validator_ref-registered callback: same polymorphic-dispatch trick as
    // GoalSlot's `validator` field (called with a STRING-kind Value wrapping
    // the raw input from validateAndNormalize, or a STRUCT-kind Value from
    // validateObject) - one callback signature covers both entry points,
    // exactly as it did in GoalState.cpp.
    using CustomValidator = std::function<ParameterValidationResult(const google::protobuf::Value&)>;

    // `name` must match a Parameter.validator_ref string exactly.
    void registerValidator(const std::string& name, CustomValidator fn);

    // Validates+normalizes `raw` against `parameter.type()`/`constraints()`.
    // If `parameter.validator_ref()` is set, looks it up and calls it instead
    // of the built-in rules below - throws std::runtime_error if the name
    // isn't registered (a wiring error, not a validation failure).
    //
    //   STRING              passed through
    //   PHONE               normalized to NXX-NXX-XXXX (US) or NXX-XXXX
    //   EMAIL               basic '@'/domain format check
    //   DATE / TIME / DATETIME   ISO 8601 / HH:MM; constraints["mustBeFuture"] (bool)
    //   NUMBER / INTEGER    constraints["min"]/["max"] (number)
    //   BOOLEAN             yes/no/true/false/1/0 (case-insensitive)
    //   ENUM                case-insensitive match against constraints["allowedValues"] (list of string)
    //   OBJECT              `raw` parsed as JSON, then validated as-if by validateObject()
    //   COMPOSITE           always fails - use dot-path parameter names instead, one sub-value at a time
    ParameterValidationResult validateAndNormalize(const Parameter& parameter, const std::string& raw) const;

    // Whole-object validation for OBJECT-typed parameters against
    // constraints["objectSchema"] (list of {name, description, jsonType, required}).
    // `jsonType` is one of "string"/"number"/"integer"/"boolean"; an unknown
    // jsonType is accepted without a type check, matching GoalState's
    // validateObject(). `obj` must be a STRUCT-kind Value.
    ParameterValidationResult validateObject(const Parameter& parameter, const google::protobuf::Value& obj) const;

    // parameter.required() && (condition_cel empty || it evaluates truthy
    // against `activation`) - ports GoalState::isRequired()'s
    // `slot.required && (!slot.condition || slot.condition(state))` exactly.
    // Throws std::runtime_error if condition_cel is malformed or doesn't
    // evaluate to a bool. Milestone 3.4's follow-up: a condition_cel
    // referencing a session field that isn't set yet throws
    // FieldNotPresentError (ExpressionEvaluator.h - a std::runtime_error
    // subtype), which this treats as "not required yet" (false) rather than
    // letting it propagate - the same "absent field means this condition
    // isn't satisfied" treatment OrchestratorEngine::evaluateRoutesOnce()'s
    // condition-route loop already gives route conditions in the identical
    // shape. Every other std::runtime_error (malformed CEL, non-bool result)
    // still propagates and crashes loudly, unchanged.
    static bool isRequired(const Parameter& parameter, const ExpressionEvaluator::Activation& activation);

private:
    std::map<std::string, CustomValidator> _customValidators;
};

} // namespace orchestrator
} // namespace va
