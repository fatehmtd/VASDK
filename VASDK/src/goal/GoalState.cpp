#include <VASDK/goal/GoalState.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace va {
namespace goal {

//  Construction 

GoalState::GoalState(GoalDefinition def)
    : _definition(std::move(def)), _slots(_definition.slots) {}

//  Slot lookup 

GoalSlot* GoalState::findSlot(const std::string& dotPath) {
    auto dot = dotPath.find('.');
    const std::string head = (dot == std::string::npos) ? dotPath : dotPath.substr(0, dot);
    const std::string tail = (dot == std::string::npos) ? "" : dotPath.substr(dot + 1);

    for (auto& slot : _slots) {
        if (slot.name != head) continue;
        if (tail.empty()) return &slot;
        if (slot.type != SlotType::COMPOSITE) return nullptr;
        for (auto& sub : slot.subSlots) {
            if (sub.name == tail) return &sub;
        }
        return nullptr;
    }
    return nullptr;
}

const GoalSlot* GoalState::findSlot(const std::string& dotPath) const {
    return const_cast<GoalState*>(this)->findSlot(dotPath);
}

//  Requirement / collection helpers 

bool GoalState::isRequired(const GoalSlot& slot) const {
    if (!slot.required) return false;
    if (slot.condition) return slot.condition(*this);
    return true;
}

bool GoalState::allRequiredSubSlotsCollected(const GoalSlot& slot) const {
    for (const auto& sub : slot.subSlots) {
        if (sub.required && !sub.value.has_value()) return false;
    }
    return true;
}

bool GoalState::isCollected(const GoalSlot& slot) const {
    if (slot.type == SlotType::COMPOSITE) return allRequiredSubSlotsCollected(slot);
    return slot.value.has_value();
}

//  Validation 

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string stripNonDigits(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) out += c;
    }
    return out;
}

std::string todayISO() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

bool isValidISODate(const std::string& s) {
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int i : {0,1,2,3,5,6,8,9}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool isValidISOTime(const std::string& s) {
    // HH:MM or H:MM
    if (s.size() < 4 || s.size() > 5) return false;
    auto colon = s.find(':');
    if (colon == std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == colon) continue;
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

} // namespace

SlotValidationResult GoalState::validateAndNormalize(const GoalSlot& slot,
                                                      const std::string& raw) const {
    if (slot.validator) {
        return slot.validator(nlohmann::json(raw));
    }

    switch (slot.type) {

    case SlotType::STRING:
        return {true, nlohmann::json(raw), ""};

    case SlotType::PHONE: {
        std::string digits = stripNonDigits(raw);
        if (digits.size() == 11 && digits[0] == '1') digits = digits.substr(1);
        if (digits.size() == 10) {
            std::string formatted = digits.substr(0,3) + "-" + digits.substr(3,3) + "-" + digits.substr(6);
            return {true, nlohmann::json(formatted), ""};
        }
        if (digits.size() == 7) {
            std::string formatted = digits.substr(0,3) + "-" + digits.substr(3);
            return {true, nlohmann::json(formatted), ""};
        }
        return {false, {}, "phone number must be 7 or 10 digits (got " + std::to_string(digits.size()) + ")"};
    }

    case SlotType::EMAIL: {
        auto at = raw.find('@');
        if (at == std::string::npos || at == 0)
            return {false, {}, "email must contain '@'"};
        auto dot = raw.find('.', at + 1);
        if (dot == std::string::npos || dot == raw.size() - 1)
            return {false, {}, "email must have a domain (e.g. user@example.com)"};
        return {true, nlohmann::json(raw), ""};
    }

    case SlotType::DATE: {
        if (!isValidISODate(raw))
            return {false, {}, "date must be in ISO 8601 format YYYY-MM-DD (got '" + raw + "')"};
        if (slot.constraints.mustBeFuture && raw <= todayISO())
            return {false, {}, "date must be in the future (got '" + raw + "')"};
        return {true, nlohmann::json(raw), ""};
    }

    case SlotType::TIME: {
        // Normalise to HH:MM
        std::string t = raw;
        if (!isValidISOTime(t))
            return {false, {}, "time must be in HH:MM format (got '" + raw + "')"};
        if (t.size() == 4) t = "0" + t; // H:MM → HH:MM
        return {true, nlohmann::json(t), ""};
    }

    case SlotType::DATETIME: {
        // Accept any non-empty string; LLM should resolve to ISO 8601
        if (raw.empty())
            return {false, {}, "datetime must be a valid ISO 8601 datetime"};
        if (slot.constraints.mustBeFuture) {
            std::string datePrefix = raw.size() >= 10 ? raw.substr(0, 10) : raw;
            if (datePrefix <= todayISO())
                return {false, {}, "datetime must be in the future (got '" + raw + "')"};
        }
        return {true, nlohmann::json(raw), ""};
    }

    case SlotType::NUMBER: {
        try {
            double v = std::stod(raw);
            if (v < slot.constraints.min || v > slot.constraints.max) {
                return {false, {}, "value " + raw + " is outside allowed range ["
                    + std::to_string(slot.constraints.min) + ", "
                    + std::to_string(slot.constraints.max) + "]"};
            }
            return {true, nlohmann::json(v), ""};
        } catch (...) {
            return {false, {}, "'" + raw + "' is not a valid number"};
        }
    }

    case SlotType::INTEGER: {
        try {
            long long v = std::stoll(raw);
            if (static_cast<double>(v) < slot.constraints.min ||
                static_cast<double>(v) > slot.constraints.max) {
                return {false, {}, "value " + raw + " is outside allowed range"};
            }
            return {true, nlohmann::json(v), ""};
        } catch (...) {
            return {false, {}, "'" + raw + "' is not a valid integer"};
        }
    }

    case SlotType::BOOLEAN: {
        std::string lo = toLower(raw);
        if (lo == "yes" || lo == "true"  || lo == "1" || lo == "on")
            return {true, nlohmann::json(true), ""};
        if (lo == "no"  || lo == "false" || lo == "0" || lo == "off")
            return {true, nlohmann::json(false), ""};
        return {false, {}, "'" + raw + "' is not a boolean (yes/no/true/false)"};
    }

    case SlotType::ENUM: {
        std::string lo = toLower(raw);
        for (const auto& v : slot.constraints.allowedValues) {
            if (toLower(v) == lo) return {true, nlohmann::json(v), ""};
        }
        std::string allowed;
        for (size_t i = 0; i < slot.constraints.allowedValues.size(); ++i) {
            if (i) allowed += ", ";
            allowed += slot.constraints.allowedValues[i];
        }
        return {false, {}, "'" + raw + "' is not valid - must be one of: " + allowed};
    }

    case SlotType::OBJECT:
        // Scalar updateSlot called for an OBJECT slot: try to parse raw as JSON
        try {
            return validateObject(slot, nlohmann::json::parse(raw));
        } catch (...) {
            return {false, {}, "slot '" + slot.name + "' is an OBJECT type - use update_object_slot instead"};
        }

    case SlotType::COMPOSITE:
        return {false, {}, "use dot notation (e.g. '" + slot.name + ".field') for COMPOSITE slots"};
    }

    return {false, {}, "unknown slot type"};
}

SlotValidationResult GoalState::validateObject(const GoalSlot& slot,
                                                const nlohmann::json& obj) const {
    if (slot.validator) return slot.validator(obj);

    if (!obj.is_object())
        return {false, {}, "expected a JSON object for slot '" + slot.name + "'"};

    for (const auto& field : slot.constraints.objectSchema) {
        if (!field.required) continue;
        if (!obj.contains(field.name))
            return {false, {}, "missing required field '" + field.name + "' in slot '" + slot.name + "'"};

        const auto& val = obj[field.name];
        bool typeOk = false;
        if      (field.jsonType == "string")  typeOk = val.is_string();
        else if (field.jsonType == "number")  typeOk = val.is_number();
        else if (field.jsonType == "integer") typeOk = val.is_number_integer();
        else if (field.jsonType == "boolean") typeOk = val.is_boolean();
        else                                   typeOk = true; // unknown type - accept

        if (!typeOk)
            return {false, {}, "field '" + field.name + "' must be " + field.jsonType
                    + " in slot '" + slot.name + "'"};
    }
    return {true, obj, ""};
}

//  Public update API 

SlotValidationResult GoalState::updateSlot(const std::string& dotPath, const std::string& rawValue) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    GoalSlot* slot = findSlot(dotPath);
    if (!slot) {
        return {false, {}, "unknown slot '" + dotPath + "'"};
    }
    auto result = validateAndNormalize(*slot, rawValue);
    if (result.valid) {
        slot->value = result.normalizedValue;
        _summaryCacheDirty = true;
        spdlog::info("[GoalState] Slot '{}' set to: {}", dotPath, result.normalizedValue.dump());
    }
    return result;
}

SlotValidationResult GoalState::updateObjectSlot(const std::string& name, const nlohmann::json& obj) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    GoalSlot* slot = findSlot(name);
    if (!slot) {
        return {false, {}, "unknown slot '" + name + "'"};
    }
    if (slot->type != SlotType::OBJECT) {
        return {false, {}, "slot '" + name + "' is not an OBJECT type - use update_slot instead"};
    }
    auto result = validateObject(*slot, obj);
    if (result.valid) {
        slot->value = obj;
        _summaryCacheDirty = true;
        spdlog::info("[GoalState] Object slot '{}' set to: {}", name, obj.dump());
    }
    return result;
}

bool GoalState::prefillSlot(const std::string& dotPath, const nlohmann::json& value) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    GoalSlot* slot = findSlot(dotPath);
    if (!slot) {
        spdlog::warn("[GoalState] prefillSlot: unknown slot '{}'", dotPath);
        return false;
    }
    slot->value = value;
    _summaryCacheDirty = true;
    spdlog::info("[GoalState] Slot '{}' pre-filled: {}", dotPath, value.dump());
    return true;
}

//  Completion checks 

bool GoalState::isComplete() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    for (const auto& slot : _slots) {
        if (isRequired(slot) && !isCollected(slot)) return false;
    }
    return true;
}

bool GoalState::isAwaitingConfirmation() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _awaitingConfirmation;
}

void GoalState::setAwaitingConfirmation(bool v) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    _awaitingConfirmation = v;
}

int GoalState::confirmationAttempts() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _confirmationAttempts;
}

void GoalState::incrementConfirmationAttempts() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    ++_confirmationAttempts;
}

int GoalState::repromptCount() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _repromptCount;
}

void GoalState::incrementRepromptCount() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    ++_repromptCount;
}

const GoalDefinition& GoalState::definition() const {
    return _definition;
}

//  Slot value access (condition predicates) 

std::string GoalState::slotValueStr(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    const GoalSlot* slot = findSlot(name);
    if (!slot || !slot->value.has_value()) return "";
    if (slot->value->is_string()) return slot->value->get<std::string>();
    return slot->value->dump();
}

nlohmann::json GoalState::slotValueJson(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    const GoalSlot* slot = findSlot(name);
    if (!slot || !slot->value.has_value()) return nullptr;
    return *slot->value;
}

//  Prompt summary helpers 

std::string GoalState::describeMissingSlot(const GoalSlot& slot) const {
    std::string line = "- " + slot.name + ": " + slot.description;

    switch (slot.type) {
    case SlotType::ENUM: {
        std::string vals;
        for (size_t i = 0; i < slot.constraints.allowedValues.size(); ++i) {
            if (i) vals += ", ";
            vals += slot.constraints.allowedValues[i];
        }
        line += " (ENUM - must be one of: " + vals + ")";
        break;
    }
    case SlotType::DATE:
        line += " (DATE - ISO 8601 YYYY-MM-DD";
        if (slot.constraints.mustBeFuture) line += ", must be a future date";
        line += ")";
        break;
    case SlotType::TIME:
        line += " (TIME - HH:MM)";
        break;
    case SlotType::DATETIME:
        line += " (DATETIME - ISO 8601";
        if (slot.constraints.mustBeFuture) line += ", must be in the future";
        line += ")";
        break;
    case SlotType::NUMBER:
    case SlotType::INTEGER: {
        line += " (" + std::string(slot.type == SlotType::INTEGER ? "INTEGER" : "NUMBER");
        bool hasMin = slot.constraints.min != -std::numeric_limits<double>::infinity();
        bool hasMax = slot.constraints.max !=  std::numeric_limits<double>::infinity();
        if (hasMin || hasMax) {
            line += " - range: ";
            if (hasMin) line += std::to_string(static_cast<long long>(slot.constraints.min));
            else        line += "any";
            line += " to ";
            if (hasMax) line += std::to_string(static_cast<long long>(slot.constraints.max));
            else        line += "any";
        }
        line += ")";
        break;
    }
    case SlotType::BOOLEAN:
        line += " (BOOLEAN - yes/no)";
        break;
    case SlotType::PHONE:
        line += " (PHONE)";
        break;
    case SlotType::EMAIL:
        line += " (EMAIL)";
        break;
    case SlotType::OBJECT: {
        line += " (OBJECT - use update_object_slot; fields: ";
        bool first = true;
        for (const auto& f : slot.constraints.objectSchema) {
            if (!first) line += ", ";
            first = false;
            line += f.name + " (" + f.jsonType;
            line += f.required ? ", required" : ", optional";
            line += ")";
        }
        line += ")";
        break;
    }
    case SlotType::COMPOSITE: {
        // Show which sub-slots are still missing
        std::vector<std::string> done, missing;
        for (const auto& sub : slot.subSlots) {
            if (sub.value.has_value()) done.push_back(sub.name + "=" + sub.value->dump());
            else if (sub.required)     missing.push_back(sub.name);
        }
        line += " (COMPOSITE - use update_slot with dot path";
        if (!done.empty()) {
            line += "; collected: ";
            for (size_t i = 0; i < done.size(); ++i) { if (i) line += ", "; line += done[i]; }
        }
        if (!missing.empty()) {
            line += "; still needed: ";
            for (size_t i = 0; i < missing.size(); ++i) { if (i) line += ", "; line += missing[i]; }
        }
        line += ")";
        break;
    }
    default:
        break;
    }
    return line;
}

std::string GoalState::describeCollectedSlot(const GoalSlot& slot) const {
    if (slot.type == SlotType::COMPOSITE) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& sub : slot.subSlots) {
            if (sub.value.has_value()) obj[sub.name] = *sub.value;
        }
        return "- " + slot.name + ": " + obj.dump();
    }
    if (slot.value.has_value()) {
        return "- " + slot.name + ": " + slot.value->dump();
    }
    return "";
}

void GoalState::rebuildSummaryCache() const {
    std::string missing, collected;
    for (const auto& slot : _slots) {
        bool collected_ = isCollected(slot);
        if (isRequired(slot) && !collected_) {
            if (!missing.empty()) missing += "\n";
            missing += describeMissingSlot(slot);
        }
        if (collected_) {
            std::string line = describeCollectedSlot(slot);
            if (!line.empty()) {
                if (!collected.empty()) collected += "\n";
                collected += line;
            }
        }
    }
    _cachedMissing     = missing.empty()   ? "(none)"     : missing;
    _cachedCollected   = collected.empty() ? "(none yet)" : collected;
    _summaryCacheDirty = false;
}

std::string GoalState::missingSlotsSummary() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_summaryCacheDirty) rebuildSummaryCache();
    return _cachedMissing;
}

std::string GoalState::collectedSlotsSummary() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (_summaryCacheDirty) rebuildSummaryCache();
    return _cachedCollected;
}

//  Full JSON export 

nlohmann::json GoalState::collectSlotJson(const GoalSlot& slot) const {
    if (slot.type == SlotType::COMPOSITE) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& sub : slot.subSlots) {
            if (sub.value.has_value()) obj[sub.name] = *sub.value;
        }
        return obj;
    }
    return slot.value.has_value() ? *slot.value : nlohmann::json(nullptr);
}

nlohmann::json GoalState::toJson() const {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    nlohmann::json out = nlohmann::json::object();
    for (const auto& slot : _slots) {
        if (isCollected(slot)) {
            out[slot.name] = collectSlotJson(slot);
        }
    }
    return out;
}

} // namespace goal
} // namespace va
