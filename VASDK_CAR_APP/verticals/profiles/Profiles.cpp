#include "Profiles.h"

#include "Car.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>

namespace car {

Profiles::Profiles(Car& car) : Vertical(car) {
    // shortcut: kept in memory; a car keeps them per key, and syncs them to the driver's account.
    _profiles = {
        {"Guest", 21.0, "pop", 12, 0},
        {"Jordan", 22.0, "jazz", 10, 1},
        {"Riley", 19.5, "rock", 16, 0},
    };
}

nlohmann::json Profiles::json(const Profile& profile) const {
    return {{"name", profile.name}, {"guest", profile.name == "Guest"}, {"temperature", profile.temperature},
            {"genre", profile.genre}, {"volume", profile.volume}, {"seat_heat", profile.seatHeat}};
}

nlohmann::json Profiles::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    nlohmann::json names = nlohmann::json::array();
    for (const auto& profile : _profiles) names.push_back(profile.name);
    return {{"driver", json(_profiles[_current])}, {"profiles", names}};
}

void Profiles::tellConversation(const nlohmann::json& driver) const {
    const auto conversation = car().conversation();
    if (conversation.setSessionParam) {   // $session.driver, for any part of the agent
        google::protobuf::Value value;
        (void)google::protobuf::util::JsonStringToMessage(driver.dump(), &value);
        conversation.setSessionParam("driver", value);
    }
    if (conversation.fireTrigger) {
        google::protobuf::Struct payload;
        (*payload.mutable_fields())["driver"].set_string_value(driver.value("name", std::string()));
        conversation.fireTrigger("driver-changed", payload);
    }
}

nlohmann::json Profiles::command(const nlohmann::json& request) {
    const auto action = lower(text(request.value("action", nlohmann::json())));
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& profile = _profiles[_current];
        if (action == "remember") {
            if (profile.name == "Guest") return error("a guest has no profile to remember it in");
            const auto key = lower(text(request.value("key", nlohmann::json())));
            const auto value = request.value("value", nlohmann::json());
            if (key == "temperature") profile.temperature = number(value, profile.temperature);
            else if (key == "genre") profile.genre = lower(text(value));
            else if (key == "volume") profile.volume = static_cast<int>(number(value, profile.volume));
            else if (key == "seat_heat") profile.seatHeat = static_cast<int>(number(value, profile.seatHeat));
            else return error("a profile remembers temperature, genre, volume and seat_heat");
            result = json(profile);
            result["remembered"] = key;
        } else if (action == "current" || action == "status" || action.empty()) {
            return json(profile);
        } else {
            return error("unknown profile action: " + action);
        }
    }
    changed();
    return result;
}

std::vector<va::llm::Tool> Profiles::tools() {
    va::llm::Tool profile("profile", "The driver's profile: who is driving, and what they like");
    profile.addParameter({"action", "current, or remember (key: temperature, genre, volume or seat_heat; value)", "string", true});
    profile.addParameter({"key", "for remember: what to remember", "string", false});
    profile.addParameter({"value", "for remember: its value", "string", false});
    profile.onCall([this](const nlohmann::json& args) { return command(args); });
    return {profile};
}

nlohmann::json Profiles::simulate(const nlohmann::json& request) {
    if (request.value("event", std::string()) != "key_fob") return error("{\"event\": \"key_fob\", \"driver\": \"Jordan\"}");
    const auto wanted = lower(request.value("driver", std::string("Guest")));
    nlohmann::json driver;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = std::find_if(_profiles.begin(), _profiles.end(), [&](const Profile& p) { return lower(p.name) == wanted; });
        if (found == _profiles.end()) return error("no profile " + wanted);
        _current = static_cast<size_t>(found - _profiles.begin());
        driver = json(*found);
    }
    changed();
    tellConversation(driver);
    return driver;
}

} // namespace car
