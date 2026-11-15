"""Fixtures for the Python app's pytest files: the binding (vasdk), skipped when it can't load."""

import sys
from pathlib import Path

import pytest

APP = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(APP))
sys.path.insert(0, str(APP.parent / "bindings" / "python"))


@pytest.fixture(scope="session")
def vasdk():
    """The vasdk module, with the C library and its generated messages; the test is skipped without them."""
    try:
        import vasdk as module
    except Exception as error:   # the library or its messages aren't built
        pytest.skip(f"the Python binding can't load: {error}")
    return module
