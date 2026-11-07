// The phone against the agent: a call that rings cuts in and stops the music, is answered and hung
// up, and the music comes back; a call placed to a contact found by the phone's own extractor.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 2;

    t.say("play some rock");
    t.vehicle->simulate({{"vertical", "phone"}, {"event", "incoming_call"}, {"caller", "mom"}});
    t.expect(t.triggered.text == "Mom is calling. Answer it?" && t.triggered.cutsIn, "it rings, by the contact's name: " + t.triggered.text);
    t.expect(t.at("media", "playing") == false, "the music stops while it rings");
    auto reply = t.say("turn the fan up");
    t.expect(t.contains(reply, "Fan at") && t.contains(reply, "Mom is calling. Answer it?"), "a command while ringing: " + reply);
    reply = t.say("pick up");
    t.expect(t.at("phone", "state") == "active" && reply == "You're connected to Mom.", "answered: " + reply);
    t.say("hang up");
    t.expect(t.at("phone", "state") == "idle" && t.at("media", "playing") == true, "hung up, the music back");

    reply = t.say("call sam carter");
    t.expect(reply == "Calling Sam Carter." && t.at("phone", "direction") == "outgoing", "a call to a contact: " + reply);
    t.expect(t.at("media", "playing") == false, "the music stops for it too");
    t.say("hang up");
    reply = t.say("call doctor patel");
    t.expect(reply == "Calling Dr. Patel.", "another name for a contact: " + reply);
    t.say("hang up");
    reply = t.say("call the pizza place");
    t.expect(reply == "Who should I call? Say, for example, \"call Sam\".", "someone not in the contacts: " + reply);

    t.vehicle->simulate({{"vertical", "phone"}, {"event", "incoming_call"}, {"caller", "Unknown"}});
    reply = t.drive->handleNoInput().text;
    t.expect(reply == "I sent it to voicemail." && t.at("phone", "state") == "idle", "left to ring: " + reply);
    return t.done("phone");
}
