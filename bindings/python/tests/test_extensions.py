"""A page type and an entity extractor written in Python, through the C API: a quiz page that
asks on entering and answers a right guess itself, and contacts found in the user's words, said
back by name with their attributes.

    VASDK_LIBRARY=build/VASDK/libVASDK.dylib python3 bindings/python/tests/test_extensions.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402

AGENT = """
start_flow: "main"
intents { name: "quiz" examples: ["quiz", "a quiz"] }
intents { name: "call" examples: ["call", "make a call"] }
intents { name: "help" examples: ["help"] }
entity_types { name: "contact" code { extractor: "contacts" } }
goals { name: "calling" slots { name: "who" entity: "@contact" prompt: "Who should I call?" } }
flows { name: "main" start_page: "welcome"
  pages { name: "welcome" on_enter { ask { say { text: "Hi." } } }
    routes { intent: "quiz" target: "quiz" } routes { intent: "call" target: "calling" } }
  pages { name: "quiz" type: "quiz" params { [type.googleapis.com/google.protobuf.Struct] {
      fields { key: "question" value { string_value: "What colour is the sky?" } }
      fields { key: "answer" value { string_value: "blue" } } } }
    routes { intent: "help" steps { say { text: "Guess a colour." } } } }
  pages { name: "calling" form { goal: "calling" }
    routes { condition: "$form.complete" steps { say { text: "Calling $form.who on $form.who.label." } } target: "welcome" } } }
"""

CONTACTS = {"mom": ("+15550100", "Mom", "mobile"), "dentist": ("+15550199", "Doctor Lee", "work")}


def main():
    registry = vasdk.Registry()
    registry.page_type(
        "quiz", "google.protobuf.Struct",
        on_enter=lambda params, turn: turn.say(params["question"]),
        on_turn=lambda params, turn: turn.text == params["answer"] and (turn.say("Right!") or turn.goto("welcome") or True))

    @registry.entity_extractor("contacts")
    def contacts(words, text):
        return [{"start": i, "value": CONTACTS[word][0], "spoken": CONTACTS[word][1], "attributes": {"label": CONTACTS[word][2]}}
                for i, word in enumerate(words) if word in CONTACTS]

    conversation = vasdk.Agent.parse(AGENT, registry).conversation()
    conversation.start()
    reply = conversation.say("a quiz")
    assert reply.page == "quiz" and "What colour is the sky?" in reply.text, reply
    reply = conversation.say("help")
    assert "Guess a colour." in reply.text, reply   # not answered by the page type: its routes get it
    reply = conversation.say("blue")
    assert "Right!" in reply.text and reply.page == "welcome", reply

    reply = conversation.say("make a call")
    assert "Who should I call?" in reply.text, reply
    reply = conversation.say("ring my dentist")
    assert "Calling Doctor Lee on work." in reply.text, reply
    print("test_extensions: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
