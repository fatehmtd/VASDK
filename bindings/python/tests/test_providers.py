"""Providers and processors written in Python, through the C API: a whole spoken session with no
models. Typed words go to the Python LLM, its reply through a Python text processor to the Python
TTS and out of on_audio; pushed audio goes through the Python VAD to the Python STT, whose turn
the Python turn model ends. The app's context, the new commands and queries, a runtime with a
Python sentence encoder and entity model; and a text conversation's snapshot, resume, entity
values, context and triggers, a page type's turn, a test case from memory.

    VASDK_LIBRARY=build/VASDK/libVASDK.dylib python3 bindings/python/tests/test_providers.py
"""

import math
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402


def wait_for(condition, seconds=5.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.02)
    return condition()


class Seen:
    def __init__(self):
        self.lock = threading.Lock()
        self.events, self.llm_requests, self.spoken = [], [], []
        self.stt_samples = self.mic_samples = self.played = self.turns_asked = 0
        self.created = self.closed = 0

    def saw(self, part):
        with self.lock:
            return any(part in str(event) for event in self.events)


SEEN = Seen()


def loudest(pcm):
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    return max((abs(sample) for sample in samples), default=0)


def register(providers):
    @providers.vad("python")
    class Loudness:
        def __init__(self, sink, sample_rate):
            SEEN.created += 1

        def detect(self, pcm):
            return 0.9 if loudest(pcm) > 1000 else 0.0

        def close(self):
            SEEN.closed += 1

    @providers.stt("python")
    class Fixed:
        def __init__(self, sink, sample_rate):
            self.sink = sink

        def write(self, pcm):
            SEEN.stt_samples += len(pcm) // 2

        def end_of_speech(self):
            self.sink.text("what", final=False)
            self.sink.text("what time is it")

    @providers.tts("python")
    class Tone:
        def __init__(self, sink, sample_rate):
            self.sink = sink

        def synthesize(self, text):
            with SEEN.lock:
                SEEN.spoken.append(text)
            self.sink.audio(struct.pack("<1600h", *([100] * 1600)))
            self.sink.done()
            return True

    @providers.turn("python")
    class Sure:
        def __init__(self, sink, sample_rate):
            pass

        def evaluate(self, pcm):
            SEEN.turns_asked += 1
            return 0.9

    @providers.llm("python")
    class Echo:
        def __init__(self, sink, sample_rate):
            pass

        def complete(self, messages):
            with SEEN.lock:
                SEEN.llm_requests.append(messages)
            last = [message["content"] for message in messages if message["role"] == "user"][-1]
            return f"You said: {last}."


def push(session, milliseconds, amplitude):
    rate = session.input_rate
    for _ in range(milliseconds // 10):
        chunk = [int(amplitude * math.sin(2 * math.pi * 440 * i / rate)) for i in range(rate // 100)]
        session.push_audio(struct.pack(f"<{len(chunk)}h", *chunk))
        time.sleep(0.01)


def test_session(folder):
    providers = vasdk.Providers(built_in=False)
    register(providers)
    config = folder / "engine.txtpb"
    config.write_text('providers { vad { id: "python" } stt { id: "python" } tts { id: "python" } turn { id: "python" } '
                      'llm { id: "python" } }\n')
    session = vasdk.Session(config, providers, system_prompt="Be brief. The user is in {context}.")
    session.on_event(lambda event: SEEN.events.append(event))

    @session.on_audio
    def played(pcm):
        SEEN.played += len(pcm) // 2

    session.on_context(lambda: {"city": "Paris"})

    def heard(pcm):
        SEEN.mic_samples += len(pcm) // 2
    session.add_processor(vasdk.AFTER_INPUT, heard)
    session.add_processor(vasdk.BEFORE_OUTPUT, lambda pcm: pcm)
    session.add_processor(vasdk.BEFORE_SYNTHESIS, lambda text: text.replace("You said", "I heard") if "You said" in text else None)
    session.start()
    try:
        session.add_processor(vasdk.AFTER_TRANSCRIPTION, lambda text: None)
        raise AssertionError("a processor was added after start()")
    except vasdk.VasdkError:
        pass

    session.inject_text("hello")
    assert wait_for(lambda: any("I heard: hello." in said for said in SEEN.spoken)), SEEN.spoken
    assert wait_for(lambda: SEEN.played >= 1600), SEEN.played
    system = SEEN.llm_requests[0][0]
    assert system["role"] == "system" and "Be brief." in system["content"] and "Paris" in system["content"], system

    push(session, 300, 0)
    push(session, 1200, 8000)
    push(session, 1500, 0)
    assert wait_for(lambda: SEEN.saw("what time is it")), "no transcript event"
    assert SEEN.stt_samples > 0 and SEEN.mic_samples > 0 and SEEN.turns_asked > 0
    assert wait_for(lambda: any("what time is it" in str(request) for request in SEEN.llm_requests))

    assert "python" in session.describe()
    assert not session.using_fallbacks()
    assert session.next_scheduled_trigger() is None
    session.reset_conversation()
    session.push_to_talk(True)
    session.push_to_talk(False)
    session.set_follow_up_listen_ms(3000)
    session.set_backchannel({"enabled": False, "duck_ms": 1200})
    session.set_entity_values("contact", [{"value": "mom", "spoken": "Mom"}])
    session.restore_trigger_state(session.trigger_state())
    session.close()
    assert SEEN.created > 0 and SEEN.created == SEEN.closed, (SEEN.created, SEEN.closed)


def test_runtime(folder):
    providers = vasdk.Providers(built_in=False)
    register(providers)
    made = []

    @providers.sentence_encoder("python")
    class Lengths:
        def __init__(self, sink, sample_rate):
            made.append(self)

        def encode(self, sentence):
            return [float(len(sentence)), 0.0, 1.0]

    @providers.entity_model("python")
    class Nothing:
        def __init__(self, sink, sample_rate):
            made.append(self)

        def extract(self, words, labels, sensitive):
            return []

    config = folder / "runtime.txtpb"
    config.write_text('providers { vad { id: "python" } stt { id: "python" } tts { id: "python" } '
                      'sentence_encoder { id: "python" } entity_model { id: "python" } }\n')
    runtime = vasdk.Runtime(config, providers, max_sessions=2)
    assert len(made) == 2, made
    first, second = runtime.session(id="one"), runtime.session(id="two")
    assert runtime.active_sessions() == 2
    try:
        runtime.session(id="three")
        raise AssertionError("max_sessions wasn't enforced")
    except vasdk.VasdkError:
        pass
    first.close()
    assert runtime.active_sessions() == 1 and runtime.idle_provider_sets() == 1
    second.close()
    runtime.close()


AGENT = """
start_flow: "main"
session_params { name: "tier" }
entity_types { name: "contact" dynamic: true list { values { value: "nobody" } } }
intents { name: "call" examples: ["call @contact"] }
flows { name: "main" start_page: "probe"
  pages { name: "probe" type: "probe"
          routes { intent: "call" steps { say { text: "Calling $nlu.entities.contact." } } target: "where" } }
  pages { name: "where" on_enter { say { text: "You're in $context.city." } } }
}
"""


def test_conversation():
    seen = {}
    registry = vasdk.Registry()

    def enter(params, turn):
        seen.update(flow=turn.flow, page=turn.page, rendered=turn.render("Tier $session.tier"))
        turn.say("Welcome.")
        turn.ask("Who should I call?", choices=["Mom", "Dad"])
        seen["so_far"] = turn.reply_text

    def on_turn(params, turn):
        if turn.text == "phone someone":
            turn.set_text("call mom")
        return False
    registry.page_type("probe", on_enter=enter, on_turn=on_turn)
    agent = vasdk.Agent.parse(AGENT, registry)
    conversation = agent.conversation()
    conversation.set_param("tier", "gold")
    conversation.set_entity_values("contact", [{"value": "mom", "spoken": "Mom", "synonyms": ["mom"]}])
    try:
        conversation.set_entity_values("nothing", [])
        raise AssertionError("an entity that isn't dynamic took values")
    except vasdk.VasdkError:
        pass
    conversation.set_context({"city": "Paris"})

    reply = conversation.start()
    assert "Welcome." in reply.text and "Who should I call?" in reply.text and reply.expects_reply, reply
    assert [choice.label for choice in reply.choices] == ["Mom", "Dad"], reply.choices
    assert seen["flow"] == "main" and seen["page"] == "probe" and seen["rendered"] == "Tier gold", seen
    assert "Who should I call?" in seen["so_far"], seen

    reply = conversation.say("phone someone")
    assert "Calling Mom." in reply.text and "You're in Paris." in reply.text and reply.page == "where", reply

    snapshot = conversation.snapshot()
    assert snapshot.page == "where", snapshot
    again = agent.conversation()
    reply, resumed = again.resume(snapshot)
    assert resumed and reply.page == "where", (resumed, reply)

    conversation.reply_interrupted()
    assert conversation.no_input_ms() >= 0
    assert conversation.next_scheduled_trigger() is None
    assert conversation.restore_trigger_state(conversation.trigger_state())
    assert conversation.check_triggers().text == ""
    assert conversation.deliver_queued().text == ""

    passed, report = vasdk.run_test(agent, {"turns": [{"expect": {"said_contains": ["Welcome."]}}]})
    assert passed, report
    passed, report = vasdk.run_test(agent, {"turns": [{"expect": {"said_contains": ["Goodbye."]}}]})
    assert not passed and report, report


def main():
    vasdk.set_log_level("warn")
    with tempfile.TemporaryDirectory() as folder:
        test_session(Path(folder))
        test_runtime(Path(folder))
    test_conversation()
    print("test_providers: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
