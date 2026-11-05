#pragma once

#include "Vertical.h"

#include <map>
#include <mutex>

namespace car {

/**
 * Climate: temperature per side, AC, fan, defrost, recirculation, seat heating, and the cabin's
 * temperature, which heads for its setting (or for the outside, read from drive) as time passes.
 * Its tool, `climate`, takes {"action", "value", "zone"}: set_temperature, adjust_temperature, ac,
 * fan, adjust_fan, defrost, recirculation, seat_heat, auto, power, status. Zones: driver,
 * passenger, both (the default).
 */
class Climate : public Vertical {
public:
    explicit Climate(Car& car) : Vertical(car) {}
    std::string name() const override { return "climate"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    /** {"cabin_temp"} */
    nlohmann::json simulate(const nlohmann::json& command) override;
    void tick(double seconds) override;

    nlohmann::json command(const nlohmann::json& request);

private:
    nlohmann::json stateLocked() const;
    double clampTemperature(double value) const;
    std::vector<std::string> zonesOf(const nlohmann::json& zone) const;

    mutable std::mutex _mutex;
    bool _power = true;
    bool _ac = false;
    bool _auto = false;
    std::map<std::string, double> _temperature{{"driver", 21.0}, {"passenger", 21.0}};
    int _fan = 3;   // 1 to 7
    bool _defrost = false;
    bool _recirculation = false;
    std::map<std::string, int> _seatHeat{{"driver", 0}, {"passenger", 0}};   // 0 to 3
    double _cabin = 24.0;
    const double _minTemperature = 16.0;
    const double _maxTemperature = 30.0;
};

} // namespace car
