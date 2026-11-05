// Navigation against the agent: a place from the map's own extractor, three routes picked by
// their place in the list, guidance at the car's speed, the arrival, and a warning when the fuel
// won't last (two verticals' state in one trigger).

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 2;

    auto reply = t.say("take me to the airport");
    t.expect(reply == "To City Airport, the fastest way is 37 minutes by the highway. Take it?", "three routes, the fastest said: " + reply);
    t.expect(t.at("navigation", "options").size() == 3, "all three in the car's state, for the screen");
    reply = t.say("the second one");
    t.expect(t.at("navigation", "route") == "shortest" && t.contains(reply, "by Main Street"), "the second choice: " + reply);
    reply = t.say("how long");
    t.expect(t.contains(reply, "to City Airport: 34.2 kilometers"), "the ETA: " + reply);

    t.vehicle->simulate({{"vertical", "drive"}, {"gear", "D"}, {"speed_kmh", 120}});
    for (int i = 0; i < 60; ++i) t.vehicle->tick(1.0);
    t.expect(car::number(t.at("navigation", "remaining_km"), 99.0) < 34.2 - 1.9, "a minute at 120 is 2 km nearer");
    t.vehicle->simulate({{"vertical", "navigation"}, {"remaining_km", 0}});
    reply = t.drive->checkTriggers().text;
    t.expect(reply == "You've arrived at City Airport.", "arrived: " + reply);

    // Not enough fuel for the trip
    reply = t.say("navigate");
    t.expect(reply == "Where to?", "no place named: asked: " + reply);
    reply = t.say("harbor mall");
    t.expect(t.contains(reply, "To Harbor Mall"), "the place: " + reply);
    t.say("yes");
    t.vehicle->simulate({{"vertical", "drive"}, {"fuel_percent", 2}});   // 13 km of range, 19.8 km to go
    reply = t.drive->checkTriggers().text;
    // The gauge says so too: low fuel, then the route's warning
    t.expect(t.contains(reply, "You won't make it to Harbor Mall on the fuel left. Add a gas station on the way?"), "the warning: " + reply);
    reply = t.say("yes");
    t.expect(t.contains(reply, "Added a stop at the nearest gas station"), "a stop added: " + reply);
    t.expect(t.drive->checkTriggers().text.empty(), "warned once");
    reply = t.say("stop navigation");
    t.expect(reply == "Navigation off." && t.at("navigation", "state") == "idle", "stopped: " + reply);

    t.vehicle->control({{"vertical", "navigation"}, {"action", "navigate"}, {"destination", "home"}});
    t.expect(t.at("navigation", "state") == "guiding", "an LLM's way: straight there");
    return t.done("navigation");
}
