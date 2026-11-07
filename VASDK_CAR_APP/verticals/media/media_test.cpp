// Media against the agent: a video only while parked (a rule on drive's state), the music stops
// for a call and comes back after (the phone's signals), and an LLM's way of asking.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 2;

    t.say("play some jazz");
    t.expect(t.at("media", "genre") == "jazz" && t.at("media", "playing") == true, "jazz from the library");

    t.vehicle->simulate({{"vertical", "drive"}, {"gear", "D"}, {"speed_kmh", 60}});
    auto reply = t.say("play a movie");
    t.expect(reply == "Videos only play while the car is parked." && t.at("media", "video") == false, "no video while moving: " + reply);
    t.vehicle->simulate({{"vertical", "drive"}, {"gear", "P"}});
    reply = t.say("play a movie");
    t.expect(reply == "Playing a video on the screen." && t.at("media", "video") == true, "a video when parked: " + reply);
    t.say("stop the video");
    t.say("resume");

    // The phone's signals
    t.vehicle->emit("call.started");
    t.expect(t.at("media", "playing") == false, "paused for a call");
    reply = t.say("next song");
    t.expect(reply == "I can't do that right now.", "no music during the call: " + reply);
    t.vehicle->emit("call.ended");
    t.expect(t.at("media", "playing") == true, "back after the call");

    t.vehicle->control({{"vertical", "media"}, {"action", "play"}, {"genre", "chill"}});
    t.expect(t.at("media", "genre") == "chill", "{action: play, genre: chill} plays chill");
    return t.done("media");
}
