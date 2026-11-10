"""A spoken session from Python (vasdk.Session), with the speech models on this machine and no
LLM: the kitchen's agent and tools, a trigger fired from Python, the context and a session param
set from Python, the assistant's audio, and a second session's speech pushed in as the user's.
Exits 77 (skipped) when the providers library or the models aren't there.

    VASDK_LIBRARY=build/VASDK/Release/VASDK.dll python VASDK_PYTHON_APP/tests/test_session.py [models dir]
"""

import os
import sys
import tempfile
import threading
import time
from pathlib import Path

APP = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(APP))
sys.path.insert(0, str(APP.parent / "bindings" / "python"))

import vasdk  # noqa: E402
from kitchen import Kitchen  # noqa: E402

failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


class Heard:
    """The session's final replies and transcripts, and the audio it gave out."""

    def __init__(self):
        self.lock = threading.Condition()
        self.replies, self.transcripts, self.tools = [], [], []
        self.samples = 0

    def event(self, event):
        with self.lock:
            kind = event.WhichOneof("kind")   # a vasdk.Event
            if kind == "agent_reply" and event.agent_reply.is_final:
                self.replies.append(event.agent_reply.text)
            elif kind == "transcript" and event.transcript.is_final:
                self.transcripts.append(event.transcript.text)
            elif kind == "tool_call":
                self.tools.append(event.tool_call.name)
            self.lock.notify_all()

    def audio(self, pcm):
        with self.lock:
            self.samples += len(pcm) // 2

    def wait(self, found, timeout=30):
        """Until found(self) holds."""
        end = time.monotonic() + timeout
        with self.lock:
            while not found(self):
                left = end - time.monotonic()
                if left <= 0:
                    return False
                self.lock.wait(left)
            return True


def main():
    models = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("VASDK_MODELS_DIR", APP.parent / "build" / "VASDK_3P_IMPL" / "models"))
    if not (models / "silero_vad.onnx").exists() or not (models / "phonon_tts").exists() or not (models / "whistle").exists():
        print(f"skipped: the speech models aren't in {models}")
        return 77
    vasdk.set_log_level("warn")
    try:
        providers = vasdk.Providers()
    except vasdk.VasdkError as error:
        print(f"skipped: {error}")
        return 77

    config = Path(tempfile.mkdtemp()) / "engine.txtpb"
    # The end of each spoken turn: turn-1-mini (by its id, through the binding) when its model is there
    turn = ' turn { id: "turn-1-mini" }' if (models / "turn_1_mini.onnx").exists() else ""
    print("end of turn: " + ("turn-1-mini" if turn else "the VAD's silence"))
    config.write_text('providers { vad { id: "silero" } stt { id: "whistle" } tts { id: "phonon" }' + turn + ' }\n', encoding="utf-8")
    kitchen = Kitchen()
    registry = vasdk.Registry()
    kitchen.register(registry)
    agent = vasdk.Agent.load(APP / "agent" / "kitchen.txtpb", registry)
    heard = Heard()
    session = vasdk.Session(config, providers, agent, id="kitchen-test", models_dir=models,
                            system_prompt="You're a kitchen assistant. The kitchen: {context}")
    kitchen.register_llm_tools(session)   # no LLM here to call them: they register
    session.on_event(heard.event)
    session.on_audio(heard.audio)
    kitchen.on_timer_done = lambda label: session.fire_trigger("timer-done", {"label": label})
    session.start()
    kitchen.attach(session)

    check(heard.wait(lambda h: any("kitchen assistant" in r for r in h.replies)), "the agent greets")
    session.inject_text("set a timer for 10 minutes")
    check(heard.wait(lambda h: "Timer set for 10 minutes." in h.replies), "a typed turn runs the Python tool: " + str(heard.replies[-1:]))
    check("timers" in heard.tools, "and the tool call is an event")
    session.fire_trigger("timer-done", {"label": "pasta"})
    check(heard.wait(lambda h: any("pasta" in r for r in h.replies)), "a trigger fired from Python is said")
    session.set_context(kitchen.context())
    session.set_param("cooking", kitchen.cooking.start("pancakes"))   # as the LLM's start_recipe tool does
    session.inject_text("next")
    check(heard.wait(lambda h: any(r.startswith("Step 1 of 6") for r in h.replies)), "a session param set from Python is the agent's")
    check(heard.wait(lambda h: h.samples > 16000), f"the assistant's speech comes out as audio ({heard.samples} samples)")

    # A timer ringing the moment a reply starts: the reply is cut off, and the timer is still heard
    # (a voice that drops what comes right after a cancel would leave it silent)
    replies = len(heard.replies)
    session.inject_text("help")
    heard.wait(lambda h: len(h.replies) > replies)
    session.fire_trigger("timer-done", {"label": "rice"})
    check(heard.wait(lambda h: any("rice" in r for r in h.replies)), "a timer rings over a reply")
    before = heard.samples
    check(heard.wait(lambda h: h.samples > before + 8000, 20), f"and is said aloud ({heard.samples - before} samples after it)")

    # The user's voice: a second session speaks, and its audio is pushed in, at a microphone's pace
    spoken, ended = [], threading.Event()
    cook = vasdk.Session(config, providers, None, id="cook", models_dir=models)
    cook.on_audio(spoken.append)
    cook.on_event(lambda e: ended.set() if e.WhichOneof("kind") == "agent_state" and e.agent_state.state == "LISTENING" and spoken
                  else None)
    cook.start()
    cook.say("What can I make?")
    ended.wait(20)
    pcm = b"".join(spoken) + b"\0" * 32000
    chunk = session.input_rate // 10 * 2
    for start in range(0, len(pcm), chunk):
        session.push_audio(pcm[start:start + chunk])
        time.sleep(0.05)
    check(heard.wait(lambda h: any("make" in t.lower() for t in h.transcripts), 30), f"pushed audio is heard: {heard.transcripts[-1:]}")
    check(heard.wait(lambda h: any("I know how to make" in r for r in h.replies), 30), "and answered")
    check(isinstance(session.snapshot(), vasdk.ConversationSnapshot), "a snapshot")
    cook.close()
    session.close()
    kitchen.timers.cancel()
    print(f"session: {'all passed' if not failures else f'{len(failures)} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
