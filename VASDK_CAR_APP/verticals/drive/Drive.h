#pragma once

#include "Vertical.h"

#include <mutex>

namespace car {

/**
 * The car on the road: speed, gear, fuel and the outside temperature. Read-only to the driver:
 * its sensors move in the simulator. Others read it (Car::read("drive", ...)): navigation drives
 * at its speed, the climate warms toward the outside, media won't play video while moving.
 * Fuel goes down as it drives.
 */
class Drive : public Vertical {
public:
    explicit Drive(Car& car) : Vertical(car) {}
    std::string name() const override { return "drive"; }
    nlohmann::json state() const override;
    nlohmann::json control(const nlohmann::json& command) override;
    /** {"speed_kmh", "gear", "fuel_percent", "outside_temp"}, any of them. */
    nlohmann::json simulate(const nlohmann::json& command) override;
    void tick(double seconds) override;

private:
    mutable std::mutex _mutex;
    double _speed = 0.0;          // km/h
    std::string _gear = "P";      // P, R, N, D
    double _fuel = 62.0;          // percent
    double _outside = 27.0;       // Celsius
    const double _kmPerPercent = 6.5;
};

} // namespace car
