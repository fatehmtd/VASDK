// Climate against the agent: what's said changes the car, the cabin offers to cool itself when
// it's hot (from $context.car.climate), and it cools with the AC on.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 2;

    t.say("set the temperature to 19");
    t.expect(t.at("climate", "driver_temp") == 19.0 && t.at("climate", "passenger_temp") == 19.0, "19 degrees both sides");
    const auto side = t.say("set the passenger side to 24");
    t.expect(t.at("climate", "driver_temp") == 19.0 && t.at("climate", "passenger_temp") == 24.0, "24 on the passenger side only");
    t.expect(t.contains(side, "24 degrees on the passenger side."), "said with its side: " + side);

    // A touch, then what the agent says next
    t.vehicle->control({{"vertical", "climate"}, {"action", "ac"}, {"value", "off"}});
    t.expect(t.say("turn off the ac") == "The AC is already off.", "the touch is what the agent reads");

    // A hot cabin: asked first, cooled on a yes
    t.vehicle->simulate({{"vertical", "climate"}, {"cabin_temp", 31}});
    auto reply = t.drive->checkTriggers();
    t.expect(reply.text == "It's 31 degrees in here. Want me to cool it down?", "the hot cabin offer: " + reply.text);
    const auto cooled = t.say("yes please");
    t.expect(t.at("climate", "ac") == true && t.contains(cooled, "AC on, at 19 degrees"), "cooled on the yes: " + cooled);
    t.expect(t.drive->checkTriggers().text.empty(), "offered once");

    // An LLM's way of saying it
    t.vehicle->control({{"vertical", "climate"}, {"action", "set_temperature"}, {"temperature", 23}});
    t.expect(t.at("climate", "driver_temp") == 23.0, "{temperature: 23} sets it");

    const double before = t.at("climate", "cabin_temp").get<double>();
    for (int i = 0; i < 60; ++i) t.vehicle->tick(1.0);
    t.expect(t.at("climate", "cabin_temp").get<double>() < before, "the AC cools the cabin");

    // The LLM's shorthand, the action as the key: done, not taken for "status"
    const double set = car::number(t.at("climate", "driver_temp"), 0);
    auto tools = t.vehicle->find("climate")->tools();
    const auto result = tools.front().execute(nlohmann::json{{"adjust_temperature", -1.0}});
    t.expect(result.value("changed", false) && car::number(t.at("climate", "driver_temp"), 0) == set - 1,
             "{\"adjust_temperature\": -1} lowers it: " + result.dump());
    return t.done("climate");
}
