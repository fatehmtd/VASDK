// The companion against the whole car, mid-chat as the app runs it: once the LLM is telling a
// story or playing a game, its words stay with the LLM, while the car's commands still work.
// Nothing is a mode: there's no page to get stuck on.

#include "CarTest.h"

int main(int argc, char** argv) {
    car::CarTest t;
    if (!t.load(argc, argv)) return 1;
    auto turn = [&t](const std::string& words) { return t.drive->handleText(words); };
    // What the app does after each turn the LLM took (DialogProcessor): the stricter rules apply
    auto llmTook = [&t](const va::flow::Reply& reply) { t.drive->setLlmHasConversation(!reply.matched || reply.toLlm); };

    auto reply = turn("tell me a story about a dragon");
    t.expect(reply.toLlm && t.drive->pageName() == "ready", "a story is the LLM's, and the conversation stays on the start page");
    llmTook(reply);

    // Mid-story: going on with it is the LLM's, not the radio's "next"
    const auto playing = t.at("media", "now_playing");
    reply = turn("what happens next");
    t.expect(reply.toLlm && t.at("media", "now_playing") == playing, "\"what happens next\" goes on with the story");
    llmTook(reply);
    reply = turn("yes");
    t.expect(!reply.matched, "\"yes\" to \"want to hear more?\" is the LLM's");
    llmTook(reply);

    // The car's commands still work in the middle, and the story isn't in the way
    const auto before = car::number(t.at("climate", "driver_temp"), 0);
    reply = turn("I'm cold");
    t.expect(reply.matched && !reply.toLlm && car::number(t.at("climate", "driver_temp"), 0) > before, "\"I'm cold\" warms the car");
    llmTook(reply);
    for (const char* cooler : {"make it a bit cooler in here", "it's getting warm in here"}) {
        const auto warm = car::number(t.at("climate", "driver_temp"), 0);
        reply = turn(cooler);
        t.expect(reply.matched && car::number(t.at("climate", "driver_temp"), 0) < warm,
                 std::string("\"") + cooler + "\" is climate's, mid-story: not left to the LLM to get the direction right");
        llmTook(reply);
    }
    reply = turn("next song");
    t.expect(reply.matched && !reply.toLlm && t.at("media", "now_playing") != playing, "\"next song\" is the radio's");
    llmTook(reply);
    reply = turn("can we play classical music");
    t.expect(reply.matched && t.at("media", "genre") == "classical", "and so is a genre, said in a sentence");
    llmTook(reply);

    // A game: the answers are the LLM's, not the phone's or navigation's
    reply = turn("let's play trivia");
    t.expect(reply.toLlm, "\"let's play trivia\" is the LLM's, not media's \"play\"");
    llmTook(reply);
    for (const char* answer : {"the answer is a whale", "Paris", "Albert Einstein"}) {
        reply = turn(answer);
        t.expect(!reply.matched && t.drive->pageName() == "ready", std::string("\"") + answer + "\" is an answer for the LLM");
        llmTook(reply);
    }
    t.expect(t.at("phone", "state") == "idle", "and nothing was answered on the phone");
    reply = turn("next question");
    t.expect(reply.toLlm, "\"next question\" is the game's");

    return t.done("companion");
}
