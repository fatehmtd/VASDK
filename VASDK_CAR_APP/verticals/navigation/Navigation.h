#pragma once

#include "Vertical.h"

#include <mutex>
#include <optional>

namespace car {

/**
 * Navigation: places found in what's said (the @place entity, by a code extractor, "places"),
 * three routes to choose from, and guidance that moves on at the car's speed (read from drive)
 * until it arrives. Its tool, `navigation`, takes {"action", "value"}: plan (value: a place),
 * start (value: fastest, shortest or no_tolls), stop, add_stop (value: a place), status.
 */
class Navigation : public Vertical {
public:
    explicit Navigation(Car& car);
    std::string name() const override { return "navigation"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    void registerExtensions(va::flow::FlowRegistry& registry) override;
    /** {"remaining_km": 2}: jump ahead on the route. */
    nlohmann::json simulate(const nlohmann::json& command) override;
    void tick(double seconds) override;

    nlohmann::json command(const nlohmann::json& request);

    struct Place { std::string value, name; double km = 0; std::vector<std::string> aliases; };
    struct Option { std::string id, label, via; double km = 0; int minutes = 0; };

private:
    std::optional<Place> place(const std::string& said) const;
    std::vector<Option> optionsTo(const Place& place) const;
    nlohmann::json stateLocked() const;
    std::vector<va::flow::FlowRegistry::EntityFound> findPlaces(const std::vector<std::string>& words) const;

    mutable std::mutex _mutex;
    std::vector<Place> _places;       // set once, read without the lock
    std::string _state = "idle";      // idle, planned, guiding, arrived
    std::optional<Place> _destination;
    std::vector<Option> _options;     // while planned
    std::optional<Option> _route;     // while guiding
    double _remaining = 0.0;          // km
    std::vector<std::string> _stops;
    bool _refuelStop = false;
};

} // namespace car
