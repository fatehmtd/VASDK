"""A kitchen assistant on VASDK, in Python.

    python app.py            talk to it: this computer's microphone and speaker (pip install sounddevice)
    python app.py --type     type to it; it answers aloud, and in the terminal
    python app.py --text     the agent alone, by text: no speech models, no audio, no LLM
    python app.py --demo     a scripted cook talks to it in a voice of its own: the whole loop, no microphone

With an LLM (LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY in the env file), cooking questions and what the
agent doesn't understand go to it, and it can use the kitchen's tools. Ctrl+C quits.
"""

import argparse
import os
import sys
import threading
import time
from pathlib import Path

APP = Path(__file__).resolve().parent
REPO = APP.parent
sys.path.insert(0, str(APP))
if (REPO / "bindings" / "python" / "vasdk").exists():   # in the source tree: the binding beside it
    sys.path.insert(0, str(REPO / "bindings" / "python"))

import vasdk  # noqa: E402
from kitchen import Kitchen, audio  # noqa: E402

DEMO_LINES = [
    "What can I make?",
    "Let's make the garlic pasta.",
    "Next.",
    "Next.",
    "Next.",
    "Yes, please.",
    "How long is left on the timer?",
    "What can I use instead of parsley?",
    "How many grams is a cup of flour?",
    "Add the ingredients to the shopping list.",
    "Thanks!",
]


def load_env(path):
    """KEY=VALUE lines into the environment (what's already set wins): the engine config reads them."""
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def find_env(given):
    if given:
        return Path(given)
    for candidate in (Path.cwd() / ".env.local", Path.cwd() / ".env", REPO / ".env.local", REPO / ".env"):
        if candidate.exists():
            return candidate
    return None


def find_models(given):
    """The speech models: --models, VASDK_MODELS_DIR, else the build tree's download folder."""
    if given or os.environ.get("VASDK_MODELS_DIR"):
        return Path(given or os.environ["VASDK_MODELS_DIR"])
    for folder in [APP, *APP.parents]:
        for candidate in (folder / "build" / "VASDK_3P_IMPL" / "models", folder / "VASDK_3P_IMPL" / "models"):
            if candidate.is_dir():
                return candidate
    sys.exit("No speech models found: build VASDK (CMake downloads them), or pass --models <folder>.")


class Printer:
    """The session's events, as a transcript: what was heard, what was said, the tools and triggers."""

    def __init__(self, heard_label="you", show_heard=True, speaker=None, verbose=False):
        self.heard_label, self.show_heard, self.speaker, self.verbose = heard_label, show_heard, speaker, verbose
        self.replied = threading.Event()

    def __call__(self, event):
        """A vasdk.Event (vasdk/engine/events.proto)."""
        kind = event.WhichOneof("kind")
        if kind == "transcript":
            heard = event.transcript
            if heard.is_final and not heard.is_backchannel and self.show_heard and heard.text:
                print(f"{self.heard_label}: {heard.text}")
        elif kind == "agent_reply":
            reply = event.agent_reply
            if reply.is_final:
                if reply.text:
                    print(f"kitchen: {reply.text}")
                self.replied.set()
        elif kind == "tool_call":
            call = event.tool_call
            print(f"  - {call.name} {call.arguments_json}" + (f" -> {call.result_json}" if self.verbose else ""))
        elif kind == "trigger_fired":
            fired = event.trigger_fired
            print(f"  - trigger {fired.name}: {vasdk.TriggerDecision.Name(fired.decision)}")
        elif kind == "agent_state":
            if event.agent_state.state == "INTERRUPTED" and self.speaker:
                self.speaker.clear()   # talked over: what hasn't played yet is dropped
        elif kind == "error" and self.verbose:
            print(f"  ! {event.error.source}: {event.error.message}")

    def wait_for_reply(self, timeout=45.0):
        """Until the reply is said (and, with a speaker, heard)."""
        self.replied.wait(timeout)
        self.replied.clear()
        while self.speaker and self.speaker.busy(0.2):
            time.sleep(0.05)


def run_text(kitchen, agent):
    """The agent alone, by text: nothing to load, nothing to say aloud, no LLM."""
    conversation = agent.conversation()
    lock = threading.Lock()   # one turn at a time: the timers ring from their own threads

    def show(reply):
        if reply.text:
            print(f"kitchen: {reply.text}" + ("   (an LLM would answer this one: run with --type)" if reply.to_llm else ""))

    def timer_done(label):
        with lock:
            print()
            show(conversation.fire_trigger("timer-done", {"label": label}))
            print("> ", end="", flush=True)

    kitchen.on_timer_done = timer_done
    kitchen.attach(conversation)
    with lock:
        show(conversation.start())
    while True:
        try:
            words = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            return
        if words:
            with lock:
                show(conversation.say(words))


def make_session(args, kitchen, agent, providers, models, printer):
    session = vasdk.Session(args.config, providers, agent, id="kitchen", models_dir=models,
                            system_prompt=(APP / "prompts" / "system_prompt.txt").read_text(encoding="utf-8"),
                            format_instruction=(APP / "prompts" / "format_instruction.txt").read_text(encoding="utf-8"))
    kitchen.register_llm_tools(session)
    session.on_event(printer)
    kitchen.on_timer_done = lambda label: session.fire_trigger("timer-done", {"label": label})
    return session


def run_session(args, kitchen, agent):
    models = find_models(args.models)
    providers = vasdk.Providers()
    want_speaker = not args.mute and audio.available()
    if args.mode == "voice" and not audio.available():
        sys.exit("Voice needs sounddevice: pip install sounddevice. Or try --type, --text or --demo.")
    printer = Printer(heard_label="you", show_heard=args.mode != "type", verbose=args.verbose)
    session = make_session(args, kitchen, agent, providers, models, printer)
    speaker = audio.Speaker(session.output_rate) if want_speaker else None
    printer.speaker = speaker
    session.on_audio(speaker.play if speaker else (lambda pcm: None))
    print(f"Loading the speech models from {models}...")
    session.start()
    kitchen.attach(session)
    try:
        if args.mode == "voice":
            muted = (lambda: False) if args.barge_in else speaker.busy
            microphone = audio.Microphone(session.input_rate, session.push_audio, muted)
            print("Listening. Talk to it; Ctrl+C quits." + ("" if args.barge_in else " (Its mic is off while it speaks: --barge-in with headphones.)"))
            try:
                while True:
                    time.sleep(0.5)
            finally:
                microphone.close()
        elif args.mode == "type":
            printer.wait_for_reply()
            while True:
                words = input("> ").strip()
                if words:
                    session.inject_text(words)
                    printer.wait_for_reply()
        else:
            run_demo(args, session, providers, models, printer, speaker)
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        session.close()
        if speaker:
            speaker.close()


def run_demo(args, session, providers, models, printer, speaker):
    """A second session with no agent speaks the cook's lines; its audio is the assistant's
    microphone, at a microphone's pace. What the assistant heard is printed as "heard"."""
    spoken = []
    finished = threading.Event()
    cook = vasdk.Session(APP / "config" / "demo_cook.txtpb", providers, None, id="cook", models_dir=models)

    @cook.on_audio
    def collect(pcm):
        spoken.append(pcm)

    @cook.on_event
    def done(event):
        if event.WhichOneof("kind") == "agent_state" and event.agent_state.state == "LISTENING" and spoken:
            finished.set()   # the cook's line is all spoken

    cook.start()
    printer.heard_label = "  heard"
    printer.wait_for_reply()
    rate, chunk = session.input_rate, session.input_rate // 10 * 2   # 100 ms of 16-bit samples
    try:
        for line in DEMO_LINES:
            print(f"\ncook: {line}")
            spoken.clear()
            finished.clear()
            cook.say(line)
            finished.wait(20)
            pcm = b"".join(spoken) + b"\0" * rate   # and a second of silence: the turn ends
            for start in range(0, len(pcm), chunk):
                piece = pcm[start:start + chunk]
                session.push_audio(piece)
                if speaker:
                    speaker.play(piece)
                time.sleep(0.1)
            printer.wait_for_reply()
    finally:
        cook.close()


def main():
    parser = argparse.ArgumentParser(description="A kitchen assistant on VASDK, in Python.")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--text", dest="mode", action="store_const", const="text", help="the agent alone, by text: no models, no LLM")
    modes.add_argument("--type", dest="mode", action="store_const", const="type", help="type to it; it answers aloud")
    modes.add_argument("--demo", dest="mode", action="store_const", const="demo", help="a scripted cook talks to it")
    parser.set_defaults(mode="voice")
    parser.add_argument("--config", default=str(APP / "config" / "engine.txtpb"), help="the engine config")
    parser.add_argument("--env-file", help="KEY=VALUE file for the config (default: .env.local or .env, here or at the repository root)")
    parser.add_argument("--models", help="the speech models folder (default: the build tree's)")
    parser.add_argument("--list-file", help="keep the shopping list in this JSON file")
    parser.add_argument("--barge-in", action="store_true", help="keep the mic open while it speaks (headphones)")
    parser.add_argument("--mute", action="store_true", help="don't play its voice")
    parser.add_argument("--verbose", action="store_true", help="tool results and errors too")
    args = parser.parse_args()

    vasdk.set_log_level("info" if args.verbose else "warn")
    env = find_env(args.env_file)
    if env:
        load_env(env)
    kitchen = Kitchen(list_file=args.list_file)
    registry = vasdk.Registry()
    kitchen.register(registry)
    agent = vasdk.Agent.load(APP / "agent" / "kitchen.txtpb", registry)
    if args.mode == "text":
        run_text(kitchen, agent)
    else:
        run_session(args, kitchen, agent)


if __name__ == "__main__":
    main()
