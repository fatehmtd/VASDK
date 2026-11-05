#include "Climate.h"

#include "Car.h"

#include <algorithm>
#include <cmath>

namespace car {

double Climate::clampTemperature(double value) const {
    return std::clamp(std::round(value * 2.0) / 2.0, _minTemperature, _maxTemperature);   // half degrees
}

std::vector<std::string> Climate::zonesOf(const nlohmann::json& zone) const {
    const auto said = lower(text(zone));
    if (said == "driver") return {"driver"};
    if (said == "passenger") return {"passenger"};
    return {"driver", "passenger"};
}

nlohmann::json Climate::stateLocked() const {
    return {{"power", _power}, {"ac", _ac}, {"auto", _auto},
            {"driver_temp", _temperature.at("driver")}, {"passenger_temp", _temperature.at("passenger")},
            {"fan", _fan}, {"defrost", _defrost}, {"recirculation", _recirculation},
            {"seat_heat_driver", _seatHeat.at("driver")}, {"seat_heat_passenger", _seatHeat.at("passenger")},
            {"cabin_temp", std::round(_cabin * 10.0) / 10.0}, {"min_temp", _minTemperature}, {"max_temp", _maxTemperature}};
}

nlohmann::json Climate::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return stateLocked();
}

nlohmann::json Climate::command(const nlohmann::json& request) {
    nlohmann::json value;
    auto action = actionOf(request, value, {"temperature", "fan", "level"});   // {"adjust_temperature": -1} too
    // An LLM may give the value under its own name: {"action": "set_temperature", "temperature": 22}.
    for (const char* key : {"temperature", "fan", "level"})
        if (value.is_null() && request.contains(key)) value = request[key];
    if (action.empty() && request.contains("temperature")) action = "set_temperature";
    const auto zone = request.value("zone", nlohmann::json());

    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto zones = zonesOf(zone);
        bool changedNow = true;
        if (action == "set_temperature" || action == "adjust_temperature") {
            if (!value.is_number() && !value.is_string()) return error("a temperature needs a value");
            changedNow = !_power;
            _power = true;
            for (const auto& z : zones) {
                const double target = action == "set_temperature" ? number(value, _temperature[z]) : _temperature[z] + number(value, 0.0);
                changedNow = changedNow || clampTemperature(target) != _temperature[z];
                _temperature[z] = clampTemperature(target);
            }
            _auto = _auto && action == "set_temperature";
            const double set = _temperature[zones[0]];
            result["limit"] = set == _minTemperature ? "min" : set == _maxTemperature ? "max" : "";
        } else if (action == "ac") {
            const bool on = truthy(value, true);
            changedNow = on != _ac || (on && !_power);
            _ac = on;
            if (on) _power = true;
        } else if (action == "fan" || action == "adjust_fan") {
            const int target = action == "fan" ? static_cast<int>(number(value, _fan)) : _fan + static_cast<int>(number(value, 1.0));
            const int fan = std::clamp(target, 1, 7);
            changedNow = fan != _fan || !_power;
            _fan = fan;
            _auto = false;
            _power = true;
        } else if (action == "defrost") {
            const bool on = truthy(value, true);
            changedNow = on != _defrost;
            _defrost = on;
            if (on) {   // as cars do: dry air, blowing hard, from outside
                _power = true;
                _ac = true;
                _fan = std::max(_fan, 5);
                _recirculation = false;
            }
        } else if (action == "recirculation") {
            const bool on = truthy(value, true);
            changedNow = on != _recirculation;
            _recirculation = on;
        } else if (action == "seat_heat") {
            const int level = std::clamp(static_cast<int>(number(value, 2.0)), 0, 3);
            const auto seats = zone.is_null() ? std::vector<std::string>{"driver"} : zones;   // "heat my seat": the driver's
            changedNow = false;
            for (const auto& z : seats) {
                changedNow = changedNow || _seatHeat[z] != level;
                _seatHeat[z] = level;
            }
            result["level"] = level;
            result["seats"] = seats.size() == 2 ? "both seats" : seats[0] == "driver" ? "your seat" : "the passenger seat";
        } else if (action == "auto") {
            const bool on = truthy(value, true);
            changedNow = on != _auto;
            _auto = on;
            if (on) { _power = true; _ac = true; }
        } else if (action == "power") {
            const bool on = truthy(value, true);
            changedNow = on != _power;
            _power = on;
            if (!on) { _ac = false; _auto = false; _defrost = false; }
        } else if (action == "status" || action.empty()) {
            changedNow = false;
        } else {
            return error("unknown climate action: " + action);
        }
        result.update(stateLocked());
        result["changed"] = changedNow;
        result["zone"] = zones.size() == 2 ? "both" : zones[0];
        result["temperature"] = _temperature[zones[0]];
        result["where"] = zones.size() == 2 ? "" : zones[0] == "driver" ? " on your side" : " on the passenger side";
    }
    if (result.value("changed", false)) changed();
    return result;
}

std::vector<va::llm::Tool> Climate::tools() {
    va::llm::Tool climate("climate", "Controls the car's climate: temperature, AC, fan, defrost, recirculation, seat heating");
    climate.addParameter({"action", "set_temperature (value in Celsius), adjust_temperature (value: degrees to add, negative to lower), "
                                    "ac (value on/off), fan (value 1-7), adjust_fan (value +1/-1), defrost (on/off), "
                                    "recirculation (on/off), seat_heat (value 0-3), auto (on/off), power (on/off), status", "string", true});
    climate.addParameter({"value", "the action's value", "string", false});
    climate.addParameter({"zone", "driver, passenger or both (default both; for seat_heat, the driver)", "string", false});
    climate.onCall([this](const nlohmann::json& args) { return command(args); });
    return {climate};
}

nlohmann::json Climate::simulate(const nlohmann::json& request) {
    if (!request.contains("cabin_temp")) return error("{\"cabin_temp\": 31}");
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _cabin = number(request["cabin_temp"], _cabin);
    }
    changed();
    return state();
}

void Climate::tick(double seconds) {
    const double outside = number(car().read("drive", "outside_temp"), 27.0);   // no drive vertical: a warm day
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // The cabin heads for its setting with the climate on (faster with the fan up), else for
        // the outside; without the AC it can't get below the outside. A few degrees a minute.
        const double setting = (_temperature["driver"] + _temperature["passenger"]) / 2.0;
        const bool cooling = setting < _cabin;
        const double target = !_power ? outside : (cooling && !_ac) ? std::max(setting, outside) : setting;
        const double rate = _power ? 0.02 + 0.01 * _fan : 0.005;   // degrees per second
        const double before = _cabin;
        if (std::abs(target - _cabin) > 0.05) _cabin += std::clamp(target - _cabin, -rate * seconds, rate * seconds);
        moved = std::round(before * 10.0) != std::round(_cabin * 10.0);
    }
    if (moved) changed();
}

} // namespace car
