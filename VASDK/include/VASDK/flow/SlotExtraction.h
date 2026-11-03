#pragma once

#include "vasdk_export.h"
#include "Expression.h"
#include "vasdk/goal/goal.pb.h"

#include <optional>
#include <string>

namespace va {
namespace goal { class GoalState; }
namespace flow {

/**
 * @brief The part of what the user said that answers `slot`, as text for GoalState to check:
 *        - ENUM: the longest of its values found as whole words ("an oil change please" → "oil change")
 *        - INTEGER, NUMBER: the first number, in digits or words ("twenty five" → "25")
 *        - BOOLEAN: "yes" or "no" (extractYesNo)
 *        - PHONE: the digits, written or spoken ("five five five, one two..." → "555 12...")
 *        - the trimmed answer for the others
 *        nullopt when the answer holds none.
 */
VASDK_EXPORT std::optional<std::string> extractSlotValue(const vasdk::goal::Slot& slot, const std::string& text);

/** A form slot's value as $form.<slot> reads it; null while it's empty. */
VASDK_EXPORT Value slotValue(const goal::GoalState& state, const std::string& slot);

/** Yes or no in an answer ("yeah sure" → true, "nope" → false); the first one said wins. */
VASDK_EXPORT std::optional<bool> extractYesNo(const std::string& text);

} // namespace flow
} // namespace va
