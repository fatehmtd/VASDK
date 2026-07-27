#include <VASDK/goal/GoalStatePersistence.h>

#include <fstream>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace va {
namespace goal {

//  FileGoalStatePersistence 

FileGoalStatePersistence::FileGoalStatePersistence(std::string baseDir)
    : _baseDir(std::move(baseDir)) {}

std::string FileGoalStatePersistence::filePath(const std::string& sessionKey) const {
    // Sanitise key - replace path separators to prevent directory traversal
    std::string safe;
    safe.reserve(sessionKey.size());
    for (char c : sessionKey) {
        safe += (c == '/' || c == '\\' || c == ':') ? '_' : c;
    }
    return _baseDir + "/" + safe + ".json";
}

void FileGoalStatePersistence::save(const std::string& sessionKey,
                                     const nlohmann::json& state) {
    std::ofstream f(filePath(sessionKey));
    if (!f) {
        spdlog::error("[GoalStatePersistence] Cannot write to '{}'", filePath(sessionKey));
        return;
    }
    f << state.dump(2);
    spdlog::debug("[GoalStatePersistence] State saved for '{}'", sessionKey);
}

std::optional<nlohmann::json> FileGoalStatePersistence::load(const std::string& sessionKey) {
    std::ifstream f(filePath(sessionKey));
    if (!f) return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        spdlog::debug("[GoalStatePersistence] State loaded for '{}'", sessionKey);
        return j;
    } catch (const std::exception& e) {
        spdlog::warn("[GoalStatePersistence] Failed to parse state for '{}': {}", sessionKey, e.what());
        return std::nullopt;
    }
}

void FileGoalStatePersistence::clear(const std::string& sessionKey) {
    std::remove(filePath(sessionKey).c_str());
    spdlog::debug("[GoalStatePersistence] State cleared for '{}'", sessionKey);
}

//  GoalState serialisation 

namespace {

nlohmann::json serializeSlot(const GoalSlot& slot) {
    nlohmann::json j;
    j["name"] = slot.name;
    if (slot.value.has_value()) {
        j["value"] = *slot.value;
    }
    if (!slot.subSlots.empty()) {
        nlohmann::json subs = nlohmann::json::array();
        for (const auto& sub : slot.subSlots) {
            subs.push_back(serializeSlot(sub));
        }
        j["subSlots"] = subs;
    }
    return j;
}

void restoreSlotValues(GoalSlot& slot, const nlohmann::json& j) {
    if (j.contains("value") && !j["value"].is_null()) {
        slot.value = j["value"];
    }
    if (j.contains("subSlots") && !slot.subSlots.empty()) {
        const auto& subs = j["subSlots"];
        for (size_t i = 0; i < slot.subSlots.size() && i < subs.size(); ++i) {
            restoreSlotValues(slot.subSlots[i], subs[i]);
        }
    }
}

} // namespace

nlohmann::json serializeGoalState(const GoalState& state) {
    nlohmann::json j;
    j["goal"]                  = state.definition().name;
    j["repromptCount"]         = state.repromptCount();
    j["awaitingConfirmation"]  = state.isAwaitingConfirmation();
    j["confirmationAttempts"]  = state.confirmationAttempts();

    nlohmann::json slots = nlohmann::json::array();
    // Export all collected values via toJson() - the flat collected-slot map
    j["collectedSlots"] = state.toJson();
    return j;
}

GoalState deserializeGoalState(const GoalDefinition& def, const nlohmann::json& snapshot) {
    GoalState state(def);

    if (snapshot.contains("collectedSlots") && snapshot["collectedSlots"].is_object()) {
        for (const auto& [name, value] : snapshot["collectedSlots"].items()) {
            state.prefillSlot(name, value);
        }
    }

    if (snapshot.contains("repromptCount")) {
        int n = snapshot["repromptCount"].get<int>();
        for (int i = 0; i < n; ++i) state.incrementRepromptCount();
    }
    if (snapshot.contains("awaitingConfirmation")) {
        state.setAwaitingConfirmation(snapshot["awaitingConfirmation"].get<bool>());
    }
    if (snapshot.contains("confirmationAttempts")) {
        int n = snapshot["confirmationAttempts"].get<int>();
        for (int i = 0; i < n; ++i) state.incrementConfirmationAttempts();
    }

    return state;
}

} // namespace goal
} // namespace va
