#include <VASDK/goal/GoalDefinitionLoader.h>
#include <VASDK/goal/ConditionEvaluator.h>

#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace va {
namespace goal {

//  Slot parsing 

namespace {

SlotType parseSlotType(const std::string& s) {
    if (s == "STRING")    return SlotType::STRING;
    if (s == "PHONE")     return SlotType::PHONE;
    if (s == "EMAIL")     return SlotType::EMAIL;
    if (s == "DATE")      return SlotType::DATE;
    if (s == "TIME")      return SlotType::TIME;
    if (s == "DATETIME")  return SlotType::DATETIME;
    if (s == "NUMBER")    return SlotType::NUMBER;
    if (s == "INTEGER")   return SlotType::INTEGER;
    if (s == "BOOLEAN")   return SlotType::BOOLEAN;
    if (s == "ENUM")      return SlotType::ENUM;
    if (s == "OBJECT")    return SlotType::OBJECT;
    if (s == "COMPOSITE") return SlotType::COMPOSITE;
    throw std::invalid_argument("[GoalDefinitionLoader] Unknown slot type: " + s);
}

GoalSlot parseSlot(const nlohmann::json& j) {
    GoalSlot slot;
    slot.name        = j.at("name").get<std::string>();
    slot.description = j.value("description", "");
    slot.type        = parseSlotType(j.value("type", "STRING"));
    slot.required    = j.value("required", true);

    if (j.contains("condition")) {
        slot.condition = compileCondition(j["condition"]);
    }

    if (j.contains("constraints")) {
        const auto& c = j["constraints"];
        if (c.contains("allowedValues")) {
            for (const auto& v : c["allowedValues"])
                slot.constraints.allowedValues.push_back(v.get<std::string>());
        }
        if (c.contains("min"))         slot.constraints.min = c["min"].get<double>();
        if (c.contains("max"))         slot.constraints.max = c["max"].get<double>();
        if (c.contains("mustBeFuture")) slot.constraints.mustBeFuture = c["mustBeFuture"].get<bool>();
        if (c.contains("objectSchema")) {
            for (const auto& f : c["objectSchema"]) {
                ObjectField field;
                field.name        = f.at("name").get<std::string>();
                field.description = f.value("description", "");
                field.jsonType    = f.value("jsonType", "string");
                field.required    = f.value("required", true);
                slot.constraints.objectSchema.push_back(std::move(field));
            }
        }
    }

    if (j.contains("subSlots")) {
        for (const auto& sub : j["subSlots"]) {
            slot.subSlots.push_back(parseSlot(sub));
        }
    }

    return slot;
}

} // namespace

//  Public API 

GoalDefinition GoalDefinitionLoader::fromJson(const nlohmann::json& j) {
    GoalDefinition def;
    def.name      = j.at("name").get<std::string>();
    def.objective = j.value("objective", "");

    if (j.contains("slots")) {
        for (const auto& s : j["slots"]) {
            def.slots.push_back(parseSlot(s));
        }
    }
    // onComplete must be registered by the app - not serialisable
    return def;
}

GoalDefinition GoalDefinitionLoader::fromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("[GoalDefinitionLoader] Cannot open: " + path);
    nlohmann::json j;
    f >> j;
    spdlog::debug("[GoalDefinitionLoader] Loaded goal from '{}'", path);
    return fromJson(j);
}

std::vector<GoalDefinition> GoalDefinitionLoader::fromDirectory(const std::string& dir) {
    std::vector<GoalDefinition> defs;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        try {
            defs.push_back(fromFile(entry.path().string()));
        } catch (const std::exception& e) {
            spdlog::warn("[GoalDefinitionLoader] Skipping '{}': {}",
                         entry.path().string(), e.what());
        }
    }
    return defs;
}

} // namespace goal
} // namespace va
