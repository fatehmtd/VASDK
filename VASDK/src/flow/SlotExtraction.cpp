#include "flow/SlotExtraction.h"

#include "flow/CompiledAgent.h"
#include "flow/Entities.h"
#include "goal/GoalState.h"

#include <algorithm>
#include <cctype>

namespace va {
namespace flow {

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n.!?") - first + 1);
}

// The system entities need no declarations: one empty catalog serves every slot.
std::optional<EntityMatch> system(const char* name, const std::string& text,
                                  std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
    static const EntityCatalog catalog;
    return catalog.find(name, text, {}, now);
}

} // namespace

Value slotValue(const goal::GoalState& state, const std::string& slot) {
    const nlohmann::json json = state.slotValueJson(slot);
    if (json.is_boolean()) return boolValue(json.get<bool>());
    if (json.is_number()) return numberValue(json.get<double>());
    if (json.is_string()) return stringValue(json.get<std::string>());
    if (json.is_null()) return nullValue();
    if (json.is_array()) {   // a list slot: "tires and an oil change"
        Value list;
        for (const auto& item : json)
            *list.mutable_list_value()->add_values() = item.is_string() ? stringValue(item.get<std::string>()) : stringValue(item.dump());
        return list;
    }
    return stringValue(json.dump());
}

std::optional<bool> extractYesNo(const std::string& text) {
    const auto answer = system("sys.yes_no", text);
    if (!answer) return std::nullopt;
    return answer->value.bool_value();
}

std::optional<std::string> extractSlotValue(const vasdk::goal::Slot& slot, const std::string& text,
                                            std::chrono::system_clock::time_point now) {
    using Type = vasdk::goal::Slot;
    switch (slot.type()) {
    case Type::ENUM: {
        const auto words = splitWords(text);
        const std::string* best = nullptr;
        size_t bestLength = 0;
        for (const auto& value : slot.values()) {
            const auto valueWords = splitWords(value);
            if (valueWords.size() > bestLength &&
                std::search(words.begin(), words.end(), valueWords.begin(), valueWords.end()) != words.end()) {
                best = &value;
                bestLength = valueWords.size();
            }
        }
        if (!best) return std::nullopt;
        return *best;
    }
    case Type::INTEGER:
    case Type::NUMBER: {
        const auto number = system(slot.type() == Type::INTEGER ? "sys.integer" : "sys.number", text);
        if (!number) return std::nullopt;
        return toText(number->value);
    }
    case Type::BOOLEAN: {
        const auto answer = extractYesNo(text);
        if (!answer) return std::nullopt;
        return std::string(*answer ? "yes" : "no");
    }
    case Type::PHONE: {
        const auto phone = system("sys.phone", text);
        if (!phone) return std::nullopt;
        return toText(phone->value);
    }
    case Type::DATE:
    case Type::TIME:
    case Type::DATETIME:
    case Type::EMAIL: {   // "next Tuesday" → 2026-11-10, "half past three" → 15:30
        const char* entity = slot.type() == Type::DATE ? "sys.date" : slot.type() == Type::TIME ? "sys.time"
                           : slot.type() == Type::DATETIME ? "sys.datetime" : "sys.email";
        const auto value = system(entity, text, now);
        if (!value) return std::nullopt;
        return toText(value->value);
    }
    default: {
        auto trimmed = trim(text);
        if (trimmed.empty()) return std::nullopt;
        return trimmed;
    }
    }
}

} // namespace flow
} // namespace va
