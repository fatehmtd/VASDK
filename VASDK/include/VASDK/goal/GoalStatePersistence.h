#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalDefinition.h>
#include <VASDK/goal/GoalState.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace va {
namespace goal {

//  Persistence interface 

class VASDK_EXPORT GoalStatePersistence {
public:
    virtual ~GoalStatePersistence() = default;

    /// Persist the serialised state under sessionKey (e.g. session ID, user ID).
    virtual void save(const std::string& sessionKey, const nlohmann::json& state) = 0;

    /// Load a previously saved state. Returns nullopt if no state exists for the key.
    virtual std::optional<nlohmann::json> load(const std::string& sessionKey) = 0;

    /// Remove a saved state (call after goal is complete or timed out).
    virtual void clear(const std::string& sessionKey) = 0;
};

//  File-based implementation (dev / test) 

class VASDK_EXPORT FileGoalStatePersistence : public GoalStatePersistence {
public:
    /// States are written as JSON files under baseDir/<sessionKey>.json
    explicit FileGoalStatePersistence(std::string baseDir);

    void save(const std::string& sessionKey, const nlohmann::json& state) override;
    std::optional<nlohmann::json> load(const std::string& sessionKey)      override;
    void clear(const std::string& sessionKey)                               override;

private:
    std::string _baseDir;
    std::string filePath(const std::string& sessionKey) const;
};

//  GoalState serialisation helpers 

/// Serialise the current GoalState to a json object that can be stored via
/// GoalStatePersistence::save() and restored with deserializeGoalState().
VASDK_EXPORT nlohmann::json  serializeGoalState(const GoalState& state);

/// Restore a GoalState from a previously serialised snapshot.
/// The GoalDefinition (including condition lambdas) must be provided by the app -
/// only the runtime values (collected slot values, repromptCount, etc.) are persisted.
VASDK_EXPORT GoalState       deserializeGoalState(const GoalDefinition& def,
                                                  const nlohmann::json& snapshot);

} // namespace goal
} // namespace va
