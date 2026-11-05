// General knowledge against the whole car: a question about the world is handed to the session's
// LLM (target LLM), on whatever page the conversation is on, and the verticals' own questions and
// commands still come first.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 1;
    auto turn = [&t](const std::string& words) { return t.drive->handleText(words); };

    for (const char* question : {"why is the sky blue", "who invented the car", "what's the capital of Peru", "how far is the moon"}) {
        const auto reply = turn(question);
        t.expect(reply.toLlm && reply.intent == "general_question", std::string("\"") + question + "\" is the LLM's to answer");
    }
    t.expect(t.drive->pageName() == "ready", "and the conversation stays where it was");

    // The verticals' own come first
    for (const char* own : {"who is driving", "how do I pair my phone", "set the temperature to 22", "how long until we get there"}) {
        const auto reply = turn(own);
        t.expect(!reply.toLlm, std::string("\"") + own + "\" isn't general knowledge");
    }
    t.expect(car::number(t.at("climate", "driver_temp"), 0) == 22, "and the car does it");

    // Mid-directions: the question goes to the LLM, and the destination is still asked for
    turn("take me somewhere");
    t.expect(t.drive->pageName() == "where", "asked where to");
    const auto reply = turn("how far is the moon");
    t.expect(reply.toLlm && t.drive->pageName() == "where", "a question mid-directions goes to the LLM, from there");
    turn("the airport");
    t.expect(t.drive->pageName() == "routes", "and the directions go on");

    return t.done("general");
}
