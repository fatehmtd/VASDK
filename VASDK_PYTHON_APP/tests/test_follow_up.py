"""A merged turn from Python: the follow-up transcript the C API delivers keeps its flag and how many
transcripts it replaces, and the app prints it as one merged line, the parts before it as they came.
ctest runs it as vasdk_python_follow_up; the binding comes from conftest.py.
"""

import pytest


@pytest.fixture(scope="module")
def heard_line(vasdk):
    from app import heard_line as function
    return function


def delivered(vasdk, event):
    """The event as a session's on_event gets it: protobuf bytes from the C API, parsed."""
    parsed = vasdk.Event()
    parsed.ParseFromString(event.SerializeToString())
    return parsed


def test_follow_up_keeps_what_it_replaces(vasdk):
    event = vasdk.Event()
    event.transcript.text = "Book a flight to Paris"
    event.transcript.is_final = True
    event.transcript.is_follow_up = True
    event.transcript.replaces_transcripts = 2
    heard = delivered(vasdk, event).transcript
    assert heard.is_follow_up
    assert heard.replaces_transcripts == 2
    assert heard.text == "Book a flight to Paris"


def test_parts_then_one_merged_line(vasdk, heard_line):
    lines = []
    for text, follow_up, replaces in [("Book a flight", False, 0), ("to Paris", False, 0), ("Book a flight to Paris", True, 2)]:
        event = vasdk.Event()
        event.transcript.text = text
        event.transcript.is_final = True
        event.transcript.is_follow_up = follow_up
        event.transcript.replaces_transcripts = replaces
        lines.append(heard_line("you", delivered(vasdk, event).transcript))
    assert lines == ["you: Book a flight", "you: to Paris", "you (merged 2 parts): Book a flight to Paris"]
