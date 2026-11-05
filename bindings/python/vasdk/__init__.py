"""VASDK agents from Python, over the C API (VASDK/c/vasdk.h) with ctypes: no compiler needed.

    import vasdk

    registry = vasdk.Registry()

    @registry.tool("save_booking")
    def save_booking(args):
        return {"reference": args["reference"], "status": "confirmed"}

    agent = vasdk.Agent.load("agents/garage-menu/agent.txtpb", registry)
    conversation = agent.conversation()
    conversation.start()["text"]                 # "Hi, this is Acme Garage. ..."
    conversation.say("book a tire repair")["page"]   # "booking"
    vasdk.run_test_file(agent, "tests/book_service.txtpb")   # (True, "")

The library is found from VASDK_LIBRARY (a path to libVASDK.dylib, .so or VASDK.dll), else in
this package (a wheel bundles it: see bindings/python/README.md) or in ./build/VASDK. It must be a
shared build of VASDK.
"""

import ctypes
import json
import os
import re
import sys
from pathlib import Path

__all__ = ["Registry", "Turn", "Agent", "Conversation", "run_test_file", "words", "version", "VasdkError"]


class VasdkError(RuntimeError):
    """A VASDK call failed; the message says why (an agent's errors say where)."""


def _find_library():
    explicit = os.environ.get("VASDK_LIBRARY")
    if explicit:
        return explicit
    names = {"darwin": ["libVASDK.dylib"], "win32": ["VASDK.dll"]}.get(sys.platform, ["libVASDK.so"])
    here = Path(__file__).resolve().parent
    folders = [here]   # installed from a wheel: bundled in the package
    builds = [Path.cwd() / "build" / "VASDK"]
    if len(here.parents) > 2:
        builds.append(here.parents[2] / "build" / "VASDK")   # the source tree's build
    for build in builds:
        folders += [build, build / "Release"]
    for folder in folders:
        for name in names:
            if (folder / name).exists():
                return str(folder / name)
    raise VasdkError("libVASDK not found: set VASDK_LIBRARY to its path")


def _load(path):
    if sys.platform == "win32":
        os.add_dll_directory(str(Path(path).resolve().parent))   # protobuf, abseil, curl... beside it
    return ctypes.CDLL(path)


_lib = _load(_find_library())

_STEP = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
_TOOL = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
_BEFORE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
_AFTER = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
_PAGE_ENTER = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
_PAGE_TURN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
_EXTRACTOR = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
_FORMATS = {"text": 0, "json": 1, "binary": 2}


def _declare(name, result, *arguments):
    function = getattr(_lib, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


_p, _s, _i = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int
_error = ctypes.POINTER(ctypes.c_void_p)
_free = _declare("vasdk_free", None, _p)
_version = _declare("vasdk_version", _s)
_registry_create = _declare("vasdk_registry_create", _p)
_registry_free = _declare("vasdk_registry_free", None, _p)
_add_step = _declare("vasdk_registry_add_step", _i, _p, _s, _STEP, _p)
_add_tool = _declare("vasdk_registry_add_tool", _i, _p, _s, _s, _TOOL, _p)
_add_envelope = _declare("vasdk_registry_add_envelope", _i, _p, _s, _BEFORE, _AFTER, _p)
_add_page_type = _declare("vasdk_registry_add_page_type", _i, _p, _s, _s, _PAGE_ENTER, _PAGE_TURN, _p)
_add_entity_extractor = _declare("vasdk_registry_add_entity_extractor", _i, _p, _s, _EXTRACTOR, _p)
_result_set = _declare("vasdk_result_set", None, _p, _s)
_result_fail = _declare("vasdk_result_fail", None, _p, _s)
_turn_text = _declare("vasdk_turn_text", _s, _p)
_turn_lookup = _declare("vasdk_turn_lookup", _p, _p, _s)
_turn_say = _declare("vasdk_turn_say", None, _p, _s)
_turn_goto = _declare("vasdk_turn_goto", None, _p, _s)
_turn_apply = _declare("vasdk_turn_apply", _i, _p, _s, ctypes.c_size_t, _i, _error)
_agent_load = _declare("vasdk_agent_load", _p, _s, _p, _error)
_agent_parse = _declare("vasdk_agent_parse", _p, _s, ctypes.c_size_t, _i, _p, _error)
_agent_free = _declare("vasdk_agent_free", None, _p)
_conversation_create = _declare("vasdk_conversation_create", _p, _p)
_conversation_free = _declare("vasdk_conversation_free", None, _p)
_conversation_start = _declare("vasdk_conversation_start", _p, _p)
_conversation_say = _declare("vasdk_conversation_say", _p, _p, _s)
_conversation_no_input = _declare("vasdk_conversation_no_input", _p, _p)
_conversation_select_choice = _declare("vasdk_conversation_select_choice", _p, _p, _i)
_conversation_fire_trigger = _declare("vasdk_conversation_fire_trigger", _p, _p, _s, _s)
_conversation_set_param = _declare("vasdk_conversation_set_param", _i, _p, _s, _s, _error)
_conversation_lookup = _declare("vasdk_conversation_lookup", _p, _p, _s)
_run_test_file = _declare("vasdk_run_test_file", _i, _p, _s, _error)


def _take(pointer):
    """The text VASDK returned, freed."""
    if not pointer:
        return None
    try:
        return ctypes.string_at(pointer).decode("utf-8")
    finally:
        _free(pointer)


def _encode(text):
    return text.encode("utf-8") if text is not None else None


def version():
    return _version().decode()


def words(text):
    """The words of a text as agents read them: lower case; letters, digits and apostrophes."""
    return re.findall(r"[a-z0-9'\u0080-￿]+", text.lower())


class Turn:
    """The turn a step or envelope runs in; only valid inside the callback."""

    def __init__(self, handle):
        self._handle = handle

    @property
    def text(self):
        return _turn_text(self._handle).decode("utf-8")

    def lookup(self, reference):
        """The value a reference holds now ("$session.tier", "$form.day"); None when unknown."""
        return json.loads(_take(_turn_lookup(self._handle, _encode(reference))))

    def say(self, text):
        _turn_say(self._handle, _encode(text))

    def goto(self, target):
        _turn_goto(self._handle, _encode(target))

    def apply(self, outcome):
        """A StepOutcome as a dict: {"set": {"$session.x": 1}, "unset": [...], "say": [...], "goto": "page"}."""
        data = json.dumps(outcome).encode()
        error = ctypes.c_void_p()
        if not _turn_apply(self._handle, data, len(data), _FORMATS["json"], ctypes.byref(error)):
            raise VasdkError(_take(error.value))


class Registry:
    """Steps, tools, envelopes, page types and entity extractors written in Python, for the agents loaded with it."""

    def __init__(self):
        self._handle = _registry_create()
        self._callbacks = []   # kept alive as long as the registry

    def __del__(self):
        if getattr(self, "_handle", None):
            _registry_free(self._handle)
            self._handle = None

    def step(self, params_type):
        """A step for custom { [type.googleapis.com/<params_type>] {...} }: fn(params: dict, turn: Turn)."""
        def register(function):
            def callback(params_json, turn, _user):
                try:
                    function(json.loads(params_json.decode("utf-8")), Turn(turn))
                except Exception as error:   # a step that fails says nothing, as in C++
                    print(f"vasdk: step {params_type} failed: {error!r}", file=sys.stderr)
            _add_step(self._handle, _encode(params_type), self._keep(_STEP(callback)), None)
            return function
        return register

    def tool(self, name, description=""):
        """A tool for call_tool steps: fn(args: dict) -> result; raising fails the call (tool-error)."""
        def register(function):
            def callback(args_json, result, _user):
                try:
                    _result_set(result, _encode(json.dumps(function(json.loads(args_json.decode("utf-8"))))))
                except Exception as error:
                    _result_fail(result, _encode(str(error) or type(error).__name__))
            _add_tool(self._handle, _encode(name), _encode(description), self._keep(_TOOL(callback)), None)
            return function
        return register

    def envelope(self, name, before=None, after=None):
        """An envelope: before(turn) -> True when it answered the turn; after(turn) on the reply."""
        def call_before(turn, _user):
            try:
                return 1 if before(Turn(turn)) else 0
            except Exception as error:
                print(f"vasdk: envelope {name} failed: {error!r}", file=sys.stderr)
                return 0

        def call_after(turn, _user):
            try:
                after(Turn(turn))
            except Exception as error:
                print(f"vasdk: envelope {name} failed: {error!r}", file=sys.stderr)

        before_callback = self._keep(_BEFORE(call_before)) if before else _BEFORE()
        after_callback = self._keep(_AFTER(call_after)) if after else _AFTER()
        _add_envelope(self._handle, _encode(name), before_callback, after_callback, None)

    def page_type(self, name, params_type=None, on_enter=None, on_turn=None):
        """A page type for pages with type: "<name>": on_enter(params: dict, turn) when the page is
        entered; on_turn(params: dict, turn) -> True when it answered the turn (else the page's
        routes get it). params_type: the full name of its params message, None for no params."""
        def call_enter(params_json, turn, _user):
            try:
                on_enter(json.loads(params_json.decode("utf-8")), Turn(turn))
            except Exception as error:
                print(f"vasdk: page type {name} failed: {error!r}", file=sys.stderr)

        def call_turn(params_json, turn, _user):
            try:
                return 1 if on_turn(json.loads(params_json.decode("utf-8")), Turn(turn)) else 0
            except Exception as error:
                print(f"vasdk: page type {name} failed: {error!r}", file=sys.stderr)
                return 0

        enter_callback = self._keep(_PAGE_ENTER(call_enter)) if on_enter else _PAGE_ENTER()
        turn_callback = self._keep(_PAGE_TURN(call_turn)) if on_turn else _PAGE_TURN()
        if not _add_page_type(self._handle, _encode(name), _encode(params_type), enter_callback, turn_callback, None):
            raise VasdkError(f"page type {name!r} didn't register")

    def entity_extractor(self, name):
        """Finds the values of code { extractor: "<name>" } entities: fn(words: list, text: str) ->
        [{"start": 2, "length": 1, "value": ..., "spoken": "Mom", "attributes": {...}}] (start and
        length index the words). Called each time the entity is looked for: keep it quick."""
        def register(function):
            def callback(request_json, result, _user):
                try:
                    request = json.loads(request_json.decode("utf-8"))
                    _result_set(result, _encode(json.dumps(list(function(request["words"], request["text"])))))
                except Exception as error:
                    _result_fail(result, _encode(str(error) or type(error).__name__))
            if not _add_entity_extractor(self._handle, _encode(name), self._keep(_EXTRACTOR(callback)), None):
                raise VasdkError(f"entity extractor {name!r} didn't register")
            return function
        return register

    def _keep(self, callback):
        self._callbacks.append(callback)
        return callback


class Agent:
    """An agent, checked and compiled: Agent.load(path) or Agent.parse(text)."""

    def __init__(self, handle, registry):
        self._handle = handle
        self._registry = registry   # its callbacks must outlive the agent

    @classmethod
    def load(cls, path, registry=None):
        error = ctypes.c_void_p()
        handle = _agent_load(_encode(str(path)), registry._handle if registry else None, ctypes.byref(error))
        if not handle:
            raise VasdkError(_take(error.value))
        return cls(handle, registry)

    @classmethod
    def parse(cls, text, registry=None, format="text"):
        data = text.encode("utf-8") if isinstance(text, str) else bytes(text)
        error = ctypes.c_void_p()
        handle = _agent_parse(data, len(data), _FORMATS[format], registry._handle if registry else None, ctypes.byref(error))
        if not handle:
            raise VasdkError(_take(error.value))
        return cls(handle, registry)

    def conversation(self):
        return Conversation(self)

    def __del__(self):
        if getattr(self, "_handle", None):
            _agent_free(self._handle)
            self._handle = None


class Conversation:
    """One conversation's place in an agent, by text. Each turn returns the reply as a dict:
    text, segments, choices, matched, intent, event, expects_reply, ends_session, flow, page,
    tool_calls, triggers."""

    def __init__(self, agent):
        self._agent = agent
        self._handle = _conversation_create(agent._handle)

    def __del__(self):
        if getattr(self, "_handle", None):
            _conversation_free(self._handle)
            self._handle = None

    def start(self):
        return json.loads(_take(_conversation_start(self._handle)))

    def say(self, text):
        return json.loads(_take(_conversation_say(self._handle, _encode(text))))

    def no_input(self):
        return json.loads(_take(_conversation_no_input(self._handle)))

    def select_choice(self, index):
        return json.loads(_take(_conversation_select_choice(self._handle, index)))

    def fire_trigger(self, name, payload=None):
        return json.loads(_take(_conversation_fire_trigger(self._handle, _encode(name), _encode(json.dumps(payload or {})))))

    def set_param(self, name, value):
        error = ctypes.c_void_p()
        if not _conversation_set_param(self._handle, _encode(name), _encode(json.dumps(value)), ctypes.byref(error)):
            raise VasdkError(_take(error.value))

    def lookup(self, reference):
        return json.loads(_take(_conversation_lookup(self._handle, _encode(reference))))


def run_test_file(agent, path):
    """Plays a FlowTestCase file: (passed, report), the report a line per difference."""
    report = ctypes.c_void_p()
    passed = bool(_run_test_file(agent._handle, _encode(str(path)), ctypes.byref(report)))
    return passed, _take(report.value) or ""
