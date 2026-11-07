"""The kitchen's code, and every conversation in tests/*.txtpb against the agent with its Python
tools and entities. No speech models, no LLM: what an LLM would answer is checked as handed to it.

    VASDK_LIBRARY=build/VASDK/Release/VASDK.dll python VASDK_PYTHON_APP/tests/test_kitchen.py
"""

import sys
import threading
from pathlib import Path

APP = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(APP))
sys.path.insert(0, str(APP.parent / "bindings" / "python"))

import vasdk  # noqa: E402
from kitchen import Kitchen, Timers, convert, duration_text, parse_duration  # noqa: E402
from kitchen.recipes import find_mentions  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}")


def code():
    # Durations, as @sys.duration gives them and as they're said
    check(parse_duration("PT9M") == 540 and parse_duration("PT1H30M") == 5400 and parse_duration(2) == 120, "durations read")
    check(duration_text(5400) == "1 hour and 30 minutes" and duration_text(61) == "1 minute and 1 second", "durations said")

    # Conversions: volume to weight for a known ingredient, temperatures, and what can't be
    check(convert(1, "cup", "grams", "flour")["text"] == "127 grams of flour", "a cup of flour in grams")
    check(convert(180, "celsius", "fahrenheit")["amount"] == 356, "an oven temperature")
    check(convert(2, "tablespoons", "teaspoons")["amount"] == 6, "spoons")
    try:
        convert(1, "cup", "grams", "gravel")
        check(False, "an unknown ingredient between volume and weight fails")
    except ValueError:
        pass

    # Entities: the longest name first, plurals, no overlap
    found = find_mentions("let's make the garlic pasta".split(), {"garlic pasta": ("aglio_olio", "aglio e olio"), "pasta": ("p", "p")})
    check([m["value"] for m in found] == ["aglio_olio"], f"the longest name wins: {found}")
    kitchen = Kitchen()
    check([m["value"] for m in kitchen.book.find_ingredients("we need two tomatoes and an avocado".split())] == ["tomatoes", "avocados"],
          "plural and singular ingredients")

    # Cooking: the steps, the end, and a timer where a step has one
    state = kitchen.cooking.start("aglio_olio")
    check(state["active"] and state["step"] == 0, "a recipe starts before its first step")
    for _ in range(3):
        state = kitchen.cooking.next()
    check(state["timer_minutes"] == 9 and state["timer_label"] == "pasta", "step 3 offers the pasta timer")
    for _ in range(4):
        state = kitchen.cooking.next()
    check(state["done"] and not state["active"], "past the last step, it's done")

    # Timers ring on their own threads
    rang = threading.Event()
    timers = Timers(lambda label: rang.set() if label == "test" else None)
    timers.start(0.05, "test")
    check(rang.wait(2), "a timer rings")
    timers.start(60, "rice")
    check("rice" in timers.summary() and timers.cancel()["count"] == 0, "timers are listed and cancelled")


def conversations():
    tests = sorted((APP / "tests").glob("*.txtpb"))
    check(bool(tests), "there are conversation tests")
    for test in tests:
        kitchen = Kitchen()   # a fresh kitchen for each: the tools keep their state
        registry = vasdk.Registry()
        kitchen.register(registry)
        agent = vasdk.Agent.load(APP / "agent" / "kitchen.txtpb", registry)
        passed, report = vasdk.run_test_file(agent, test)
        check(passed, f"{test.name}\n{report}")
        kitchen.timers.cancel()


def main():
    vasdk.set_log_level("warn")
    code()
    conversations()
    print(f"kitchen: {'all passed' if not failures else f'{len(failures)} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
