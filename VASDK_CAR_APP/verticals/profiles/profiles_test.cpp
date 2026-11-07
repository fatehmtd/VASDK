// Profiles against the agent: a key fob sets $session.driver and the driver's settings are
// applied with the climate's and media's tools; a temperature remembered; a guest.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 2;

    t.vehicle->simulate({{"vertical", "profiles"}, {"event", "key_fob"}, {"driver", "Jordan"}});
    t.expect(t.triggered.text == "Welcome back, Jordan. 22 degrees, and some jazz.", "welcomed: " + t.triggered.text);
    t.expect(t.at("climate", "driver_temp") == 22.0 && t.at("climate", "seat_heat_driver") == 1 && t.at("media", "genre") == "jazz" &&
                 t.at("media", "volume") == 10,
             "the profile applied with the climate's and media's tools");
    auto reply = t.say("who's driving");
    t.expect(reply == "This is Jordan's profile.", "who: " + reply);
    t.say("set the temperature to 23.5");
    reply = t.say("remember this temperature");
    t.expect(reply == "I'll remember 23.5 degrees for you, Jordan.", "remembered: " + reply);
    t.say("set the temperature to 18");
    reply = t.say("my usual temperature");
    t.expect(reply == "Your usual 23.5 degrees." && t.at("climate", "driver_temp") == 23.5, "back to it: " + reply);

    t.vehicle->simulate({{"vertical", "profiles"}, {"event", "key_fob"}, {"driver", "Guest"}});
    t.expect(t.triggered.text == "Hi! You're driving as a guest.", "a guest: " + t.triggered.text);
    reply = t.say("remember this temperature");
    t.expect(reply == "Guests don't have a profile to keep that in.", "nothing to keep it in: " + reply);
    return t.done("profiles");
}
