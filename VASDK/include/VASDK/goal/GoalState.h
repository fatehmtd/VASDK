#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalDefinition.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace goal {

class VASDK_EXPORT GoalState {
public:
    explicit GoalState(GoalDefinition def);

    // std::recursive_mutex is neither copyable nor movable, so we must define these
    // explicitly and leave the new mutex in its default-constructed (unlocked) state.
    GoalState(GoalState&& o) noexcept
        : _definition(std::move(o._definition))
        , _slots(std::move(o._slots))
        , _repromptCount(o._repromptCount)
        , _awaitingConfirmation(o._awaitingConfirmation)
        , _confirmationAttempts(o._confirmationAttempts)
        , _summaryCacheDirty(o._summaryCacheDirty)
        , _cachedMissing(std::move(o._cachedMissing))
        , _cachedCollected(std::move(o._cachedCollected)) {}

    GoalState& operator=(GoalState&& o) noexcept {
        if (this != &o) {
            _definition           = std::move(o._definition);
            _slots                = std::move(o._slots);
            _repromptCount        = o._repromptCount;
            _awaitingConfirmation = o._awaitingConfirmation;
            _confirmationAttempts = o._confirmationAttempts;
            _summaryCacheDirty    = o._summaryCacheDirty;
            _cachedMissing        = std::move(o._cachedMissing);
            _cachedCollected      = std::move(o._cachedCollected);
        }
        return *this;
    }

    //  Slot updates 

    /// Update a scalar slot (STRING, PHONE, DATE, ENUM, BOOLEAN, NUMBER, …)
    /// or a COMPOSITE sub-slot via dot-path (e.g. "address.city").
    /// rawValue is a plain string extracted from speech; validated & normalised internally.
    SlotValidationResult updateSlot(const std::string& dotPath, const std::string& rawValue);

    /// Update an OBJECT-type slot directly from a parsed json object.
    SlotValidationResult updateObjectSlot(const std::string& name, const nlohmann::json& obj);

    /// Pre-fill a slot before the pipeline starts (session data, external lookup, etc.).
    /// Accepts nlohmann::json for any slot type; skips validation.
    bool prefillSlot(const std::string& dotPath, const nlohmann::json& value);

    //  Completion state 

    bool isComplete() const;

    bool isAwaitingConfirmation() const;
    void setAwaitingConfirmation(bool v);
    int  confirmationAttempts() const;
    void incrementConfirmationAttempts();

    int  repromptCount() const;
    void incrementRepromptCount();

    //  Prompt variable resolvers 

    /// Rich summary of slots still needed; respects conditions and composite/object completeness.
    std::string missingSlotsSummary() const;

    /// Summary of slots already collected; renders objects as compact JSON.
    std::string collectedSlotsSummary() const;

    //  Slot value access (used by condition lambdas) 

    /// Returns the string value of a collected scalar slot, or "" if not collected.
    std::string slotValueStr(const std::string& name) const;

    /// Returns the json value of a collected slot, or json(nullptr) if not collected.
    nlohmann::json slotValueJson(const std::string& name) const;

    /// All collected slot values as a flat json object (for onComplete callback).
    nlohmann::json toJson() const;

    const GoalDefinition& definition() const;

private:
    GoalDefinition        _definition;
    std::vector<GoalSlot> _slots; ///< Mutable copy - holds runtime values
    int  _repromptCount          = 0;
    bool _awaitingConfirmation   = false;
    int  _confirmationAttempts   = 0;

    // recursive_mutex so condition lambdas can call slotValueStr() while a
    // summary is being rebuilt (both paths acquire _mutex on the same thread).
    mutable std::recursive_mutex _mutex;

    // Lazy summary cache - invalidated whenever any slot value changes.
    mutable bool        _summaryCacheDirty = true;
    mutable std::string _cachedMissing;
    mutable std::string _cachedCollected;

    //  Internal helpers

    GoalSlot*       findSlot(const std::string& dotPath);
    const GoalSlot* findSlot(const std::string& dotPath) const;

    bool isRequired(const GoalSlot& slot) const;
    bool isCollected(const GoalSlot& slot) const;
    bool allRequiredSubSlotsCollected(const GoalSlot& slot) const;

    SlotValidationResult validateAndNormalize(const GoalSlot& slot, const std::string& raw) const;
    SlotValidationResult validateObject(const GoalSlot& slot, const nlohmann::json& obj) const;

    std::string describeMissingSlot(const GoalSlot& slot) const;
    std::string describeCollectedSlot(const GoalSlot& slot) const;

    nlohmann::json collectSlotJson(const GoalSlot& slot) const;

    void rebuildSummaryCache() const; ///< Must be called with _mutex already held.
};

} // namespace goal
} // namespace va
