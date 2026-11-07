#include "Navigation.h"

#include "Car.h"

#include <algorithm>
#include <cmath>

namespace car {

Navigation::Navigation(Car& car) : Vertical(car) {
    // shortcut: a fixed list with set distances; a head unit asks its map's geocoder and router.
    _places = {
        {"home", "home", 12.0, {"home", "my house", "my place"}},
        {"work", "work", 24.0, {"work", "my work", "the workplace"}},
        {"harbor_mall", "Harbor Mall", 18.0, {"harbor mall", "the mall", "mall"}},
        {"pine_street_cafe", "Pine Street Cafe", 6.0, {"pine street cafe", "the cafe", "pine street", "a cafe", "coffee"}},
        {"city_airport", "City Airport", 38.0, {"city airport", "the airport", "airport"}},
        {"riverside_park", "Riverside Park", 9.0, {"riverside park", "the park", "riverside"}},
        {"gas_station", "the nearest gas station", 3.2, {"gas station", "petrol station", "fuel station", "somewhere to get gas", "a gas station"}},
        {"charging_station", "the nearest charger", 5.0, {"charger", "charging station", "a charger", "somewhere to charge"}},
    };
}

std::optional<Navigation::Place> Navigation::place(const std::string& said) const {
    const auto wanted = words(said);
    for (const auto& place : _places) {
        if (words(place.value) == wanted || place.value == said || words(place.name) == wanted) return place;
        for (const auto& alias : place.aliases)
            if (words(alias) == wanted) return place;
    }
    return std::nullopt;
}

std::vector<Navigation::Option> Navigation::optionsTo(const Place& place) const {
    // Three routes, as a router would offer them: the fastest by the highway, a shorter one
    // through town, and one without tolls by the coast.
    const double km = place.km;
    return {
        {"fastest", "the fastest route", "the highway", std::round(km * 11.0) / 10.0, static_cast<int>(std::round(km * 0.9 + 3))},
        {"shortest", "the shortest route", "Main Street", std::round(km * 9.0) / 10.0, static_cast<int>(std::round(km * 1.3 + 4))},
        {"no_tolls", "the route without tolls", "Coast Road", std::round(km * 12.0) / 10.0, static_cast<int>(std::round(km * 1.2 + 5))},
    };
}

std::vector<va::flow::FlowRegistry::EntityFound> Navigation::findPlaces(const std::vector<std::string>& said) const {
    std::vector<va::flow::FlowRegistry::EntityFound> found;
    for (size_t start = 0; start < said.size();) {
        size_t best = 0;
        const Place* match = nullptr;
        for (const auto& place : _places) {
            for (const auto& alias : place.aliases) {
                const auto name = words(alias);
                if (name.size() <= best || start + name.size() > said.size()) continue;
                if (std::equal(name.begin(), name.end(), said.begin() + static_cast<long>(start))) {
                    best = name.size();
                    match = &place;
                }
            }
        }
        if (!match) {
            ++start;
            continue;
        }
        va::flow::FlowRegistry::EntityFound entity;
        entity.start = start;
        entity.length = best;
        entity.value.set_string_value(match->value);
        entity.spoken = match->name;
        (*entity.attributes.mutable_fields())["km"].set_number_value(match->km);
        found.push_back(entity);
        start += best;
    }
    return found;
}

void Navigation::registerExtensions(va::flow::FlowRegistry& registry) {
    registry.addEntityExtractor("places", [this](const va::flow::FlowRegistry::EntityRequest& request) {
        return findPlaces(request.words);
    });
}

nlohmann::json Navigation::stateLocked() const {
    nlohmann::json state{{"state", _state}, {"refuel_stop", _refuelStop}, {"stops", _stops}};
    if (_destination) {
        state["destination"] = _destination->value;
        state["destination_name"] = _destination->name;
    }
    nlohmann::json options = nlohmann::json::array();
    for (const auto& option : _options)
        options.push_back({{"id", option.id}, {"label", option.label}, {"via", option.via}, {"km", option.km}, {"minutes", option.minutes}});
    state["options"] = options;
    if (_route && (_state == "guiding" || _state == "arrived")) {
        state["route"] = _route->id;
        state["label"] = _route->label;
        state["via"] = _route->via;
        state["remaining_km"] = std::round(_remaining * 10.0) / 10.0;
        // At the route's own pace: its minutes for its kilometres.
        state["eta_min"] = static_cast<int>(std::ceil(_remaining / std::max(_route->km, 0.1) * _route->minutes));
    }
    return state;
}

nlohmann::json Navigation::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return stateLocked();
}

nlohmann::json Navigation::command(const nlohmann::json& request) {
    const auto action = lower(text(request.value("action", nlohmann::json())));
    auto value = text(request.value("value", nlohmann::json()));
    if (value.empty()) value = text(request.value("destination", nlohmann::json()));   // as an LLM may name it
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (action == "plan" || action == "navigate") {
            const auto to = place(value);
            if (!to) return error("I don't know where " + value + " is");
            _destination = to;
            _options = optionsTo(*to);
            _route.reset();
            _stops.clear();
            _refuelStop = false;
            _state = "planned";
            if (action == "navigate") {   // straight there, by the fastest route (the LLM's shortcut)
                _route = _options.front();
                _remaining = _route->km;
                _state = "guiding";
            }
        } else if (action == "start") {
            if (_options.empty()) return error("no route planned");
            const auto id = lower(value);
            auto chosen = std::find_if(_options.begin(), _options.end(), [&](const Option& o) {
                return o.id == id || lower(o.label) == id || words(o.id) == words(id);
            });
            if (chosen == _options.end()) chosen = _options.begin();
            _route = *chosen;
            _remaining = _route->km;
            _state = "guiding";
        } else if (action == "stop") {
            const bool was = _state == "guiding" || _state == "planned";
            _state = "idle";
            _destination.reset();
            _options.clear();
            _route.reset();
            _stops.clear();
            _refuelStop = false;
            result["stopped"] = was;
        } else if (action == "add_stop") {
            if (_state != "guiding") return error("not navigating");
            const auto stop = place(value);
            if (!stop) return error("I don't know where " + value + " is");
            _stops.push_back(stop->name);
            _refuelStop = _refuelStop || stop->value == "gas_station";
            result["stop_name"] = stop->name;
            result["stop_km"] = std::round(std::min(stop->km, _remaining) * 10.0) / 10.0;
        } else if (action == "status" || action.empty()) {
            result.update(stateLocked());
            return result;
        } else {
            return error("unknown navigation action: " + action);
        }
        result.update(stateLocked());
    }
    changed();
    return result;
}

std::vector<va::llm::Tool> Navigation::tools() {
    va::llm::Tool navigation("navigation", "Turn-by-turn navigation: plan routes to a place, start one, add a stop, stop");
    navigation.addParameter({"action", "navigate (value: a place, straight there), plan (value: a place: offers 3 routes), "
                                       "start (value: fastest, shortest or no_tolls), add_stop (value: a place), stop, status",
                             "string", true});
    navigation.addParameter({"value", "a place (home, work, Harbor Mall, Pine Street Cafe, City Airport, Riverside Park, "
                                      "gas station, charger) or a route", "string", false});
    navigation.onCall([this](const nlohmann::json& args) { return command(args); });
    return {navigation};
}

nlohmann::json Navigation::simulate(const nlohmann::json& request) {
    if (!request.contains("remaining_km")) return error("{\"remaining_km\": 2}");
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != "guiding") return error("not navigating");
        _remaining = std::max(0.0, number(request["remaining_km"], _remaining));
    }
    tick(0.0);   // arrives, if it's 0
    changed();
    return state();
}

void Navigation::tick(double seconds) {
    const double speed = number(car().read("drive", "speed_kmh"), 0.0);   // no drive vertical: parked
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != "guiding") return;
        const double before = std::round(_remaining * 10.0);
        _remaining = std::max(0.0, _remaining - speed * seconds / 3600.0);
        if (_remaining <= 0.0) _state = "arrived";
        moved = std::round(_remaining * 10.0) != before || _state == "arrived";
    }
    if (moved) changed();
}

} // namespace car
