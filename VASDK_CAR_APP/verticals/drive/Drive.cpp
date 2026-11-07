#include "Drive.h"

#include "Car.h"

#include <algorithm>
#include <cmath>

namespace car {

nlohmann::json Drive::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return {{"speed_kmh", std::round(_speed)}, {"gear", _gear}, {"moving", _speed > 0.5},
            {"fuel_percent", std::round(_fuel)}, {"range_km", std::round(_fuel * _kmPerPercent)}, {"outside_temp", _outside}};
}

nlohmann::json Drive::control(const nlohmann::json&) { return error("the driver drives: use the simulator"); }

nlohmann::json Drive::simulate(const nlohmann::json& command) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (command.contains("speed_kmh")) _speed = std::clamp(number(command["speed_kmh"], _speed), 0.0, 200.0);
        if (command.contains("gear")) {
            const auto gear = text(command["gear"]);
            if (gear != "P" && gear != "R" && gear != "N" && gear != "D") return error("gears are P, R, N and D");
            _gear = gear;
        }
        if (_gear == "P") _speed = 0.0;
        else if (_speed > 0.5 && _gear == "N") _gear = "D";
        if (command.contains("fuel_percent")) _fuel = std::clamp(number(command["fuel_percent"], _fuel), 0.0, 100.0);
        if (command.contains("outside_temp")) _outside = number(command["outside_temp"], _outside);
    }
    changed();
    return state();
}

void Drive::tick(double seconds) {
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_speed <= 0.5) return;
        const double before = std::round(_fuel);
        _fuel = std::max(0.0, _fuel - _speed * seconds / 3600.0 / _kmPerPercent);
        moved = std::round(_fuel) != before;
    }
    if (moved) changed();
}

} // namespace car
