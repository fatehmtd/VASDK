"""The garage agent's whole suite from Python: its C++ step, envelope and tool written again in
Python through the C API, and every test case played through them. Then a conversation by hand.

    VASDK_LIBRARY=build/VASDK/libVASDK.dylib python3 bindings/python/tests/test_garage.py VASDK_APP/agents/garage-menu
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402


def garage_extensions():
    """What extensions/GarageExtensions.cpp registers, in Python."""
    registry = vasdk.Registry()
    bookings = []

    @registry.step("garage.BookingReference")
    def booking_reference(params, turn):
        # "AG-" and four digits from the booking's values (FNV-1a), as the C++ step makes it.
        booking = "".join(f"{turn.lookup(slot) or ''}|" for slot in ("$form.service", "$form.day", "$form.time", "$form.phone"))
        value = 2166136261
        for byte in booking.encode("utf-8"):
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        turn.apply({"set": {params["into"]: f"AG-{1000 + value % 9000}"}})

    @registry.tool("save_booking", "Keeps a confirmed booking")
    def save_booking(booking):
        if not booking.get("reference"):
            raise ValueError("a booking needs a reference")
        bookings.append(booking)
        return {"reference": booking["reference"], "status": "confirmed"}

    thanks_words = {"thanks", "thank", "you", "very", "much", "cheers", "ok", "okay"}

    def thanks(turn):
        said = vasdk.words(turn.text)
        if not said or any(word not in thanks_words for word in said):
            return False   # more than thanks: "no that's all thanks"
        if any(word in ("thanks", "thank", "cheers") for word in said):
            turn.say("You're welcome.")
            return True
        return False

    registry.envelope("thanks", before=thanks)
    return registry, bookings


def main():
    folder = Path(sys.argv[1] if len(sys.argv) > 1 else "VASDK_APP/agents/garage-menu")
    registry, bookings = garage_extensions()
    agent = vasdk.Agent.load(folder / "agent.txtpb", registry)

    failed = 0
    cases = sorted((folder / "tests").glob("*.txtpb"))
    for case in cases:
        passed, report = vasdk.run_test_file(agent, case)
        print(("PASS " if passed else "FAIL ") + case.name)
        if not passed:
            failed += 1
            print("    " + report.strip().replace("\n", "\n    "))
    print(f"{len(cases) - failed} of {len(cases)} passed")

    # By hand: a booking from Python, with the Python step and tool.
    conversation = agent.conversation()
    assert "Acme Garage" in conversation.start().text
    reply = conversation.say("book a tire repair on tuesday")
    assert reply.page == "booking", reply
    conversation.say("2 pm")
    conversation.say("555 123 4567")
    reply = conversation.say("yes")
    assert reply.page == "booked" and "AG-" in reply.text, reply
    assert any(call.name == "save_booking" and call.ok for call in reply.tool_calls), reply
    assert conversation.lookup("$session.reference").startswith("AG-")
    assert bookings, "save_booking was called"
    try:
        conversation.set_param("nope", 1)
        raise AssertionError("an undeclared param must fail")
    except vasdk.VasdkError:
        pass
    try:
        vasdk.Agent.parse("flows {")
        raise AssertionError("a broken agent must fail")
    except vasdk.VasdkError as error:
        assert str(error)
    print("by hand: a booking from Python, all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
