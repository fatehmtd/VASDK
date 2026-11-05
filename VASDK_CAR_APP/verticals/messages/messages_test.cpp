// Messages against the agent: one sent from the form (who to, from the phone's contacts, and what
// it says, read back first), and one that comes in, offered, read and answered.

#include "CarTest.h"

int main(int argc, char** argv) {
    {
        car::CarTest t;
        if (!t.load(argc, argv)) return 2;
        auto reply = t.say("text sam");
        t.expect(reply == "What's the message?", "the contact named on the way in: " + reply);
        reply = t.say("running ten minutes late");
        t.expect(reply == "To Sam Carter: running ten minutes late. Send it?", "read back: " + reply);
        reply = t.say("yes");
        t.expect(reply == "Sent to Sam Carter.", "sent: " + reply);
        const auto messages = t.at("messages", "messages");
        t.expect(messages.size() == 1 && messages[0]["sent"] == true && messages[0]["body"] == "running ten minutes late", "kept as sent");

        reply = t.say("send a message");
        t.expect(reply == "Who's it for?", "no contact named: asked: " + reply);
        reply = t.say("my mom");
        t.expect(reply == "What's the message?", "then what it says: " + reply);
        t.say("see you at dinner");
        reply = t.say("no");
        t.expect(reply == "Who it's for, or what it says?", "a no: what to change: " + reply);
        if (t.failed) return t.done("messages (sent)");
    }

    car::CarTest t;   // a new car and conversation
    if (!t.load(argc, argv)) return 2;
    t.vehicle->simulate({{"vertical", "messages"}, {"event", "message"}, {"from", "Sam Carter"}, {"body", "Running late, sorry!"}});
    t.expect(t.triggered.text == "New message from Sam Carter. Want to hear it?", "offered: " + t.triggered.text);
    t.expect(t.at("messages", "unread") == 1, "unread");
    auto reply = t.say("yes");
    t.expect(reply == "Sam Carter says: Running late, sorry! Want to reply?", "read aloud: " + reply);
    t.expect(t.at("messages", "unread") == 0, "now read");
    reply = t.say("yes");
    t.expect(reply == "What's the message?", "a reply, to the sender: " + reply);
    reply = t.say("no worries");
    t.expect(reply == "To Sam Carter: no worries. Send it?", "read back: " + reply);
    reply = t.say("yes");
    t.expect(reply == "Sent to Sam Carter.", "sent: " + reply);
    reply = t.say("any new messages");
    t.expect(reply == "No new messages.", "none left: " + reply);
    return t.done("messages");
}
