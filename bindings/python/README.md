# vasdk for Python

You can write and test VASDK agents from Python. The package wraps the C API (`VASDK/c/vasdk.h`) with ctypes, so there's nothing to compile. What crosses is VASDK's protobuf messages, the ones C++ uses: a reply is a `vasdk.ConversationReply`, an event a `vasdk.Event` (see [Messages](#messages)). It needs the `protobuf` package (`pip install protobuf`). Python can supply:

- steps
- tools
- envelopes
- page types
- entity extractors

```python
import vasdk

registry = vasdk.Registry()

@registry.tool("save_booking")
def save_booking(args):
    return {"reference": "AG-1234", "status": "confirmed"}

@registry.entity_extractor("contacts")
def contacts(words, text):
    return [{"start": i, "value": "+15550100", "spoken": "Mom"} for i, word in enumerate(words) if word == "mom"]

registry.add_language("languages/fr-FR.txtpb")   # or a vasdk.Language; vasdk.builtin_language() is en-US

agent = vasdk.Agent.load("agents/garage-menu/agent.txtpb", registry)
conversation = agent.conversation()
conversation.start().text
conversation.say("book a tire repair").page
vasdk.run_test_file(agent, "tests/book_service.txtpb")   # (True, "")
```

## Spoken sessions

`vasdk.Session` is a spoken conversation: the speech providers and the LLM an [engine config](../../docs/reference/engine-config.md) names, and an agent (or `None` for the LLM alone). Your code brings the audio, as 16-bit mono PCM: a microphone, a phone line, a file.

```python
providers = vasdk.Providers()            # every VAD, STT and TTS provider built into the C library
session = vasdk.Session("engine.txtpb", providers, agent, models_dir="models",
                        system_prompt="You're a kitchen assistant. The kitchen: {context}")

@session.tool("set_timer", "Starts a kitchen timer", [{"name": "minutes", "type": "number"}])
def set_timer(args):                     # a tool the LLM may call
    return {"started": args["minutes"]}

@session.on_event
def on_event(event):                     # a vasdk.Event: transcript, agent_reply, tool_call, page_changed, ...
    if event.WhichOneof("kind") == "agent_reply" and event.agent_reply.is_final:
        print(event.agent_reply.text)

session.on_audio(speaker.play)           # the assistant's speech, at session.output_rate
session.start()
session.push_audio(pcm)                  # the user's audio, at session.input_rate
session.inject_text("set a timer for five minutes")   # or a turn by text
session.fire_trigger("timer-done", {"label": "pasta"})   # one of the agent's app_event triggers
session.set_context({"kitchen": {...}})  # $context.kitchen, and {context} in the system prompt
```

The config can also be built in code, each provider's settings its own message (`vasdk.providers.options_pb2`):

```python
from vasdk.providers.options_pb2 import PhononOptions

config = vasdk.EngineConfig()
config.providers.vad.id, config.providers.stt.id, config.providers.tts.id = "silero", "whistle", "phonon"
config.providers.tts.options.Pack(PhononOptions(voice="Zoey", temperature=0.4))
session = vasdk.Session(config, providers, agent, models_dir="models")   # vasdk.Runtime(config, ...) too
```

| | |
| --- | --- |
| `Providers()` | Adds the providers built into the C library, `vasdk_c` (with a shared VASDK instead: the `vasdk_providers` library, from `VASDK_PROVIDERS_LIBRARY`, else beside VASDK or in the build tree). |
| `Session(config, providers, agent, id, models_dir, system_prompt, format_instruction)` | Before `start()`: `on_event`, `on_audio`, `tool`. Callbacks run on VASDK's threads; keep them quick. |
| `push_audio`, `inject_text`, `say` | The user's audio, a user turn by text, words the assistant says. |
| `send(vasdk.Command(...))` | What a `Command` asks: `inject_text`, `select_choice`, `fire_trigger`, `say`, `interrupt`, `set_session_param`. `select_choice`, `interrupt` and the methods around them are shortcuts for it. |
| `fire_trigger`, `set_context`, `set_param` | The agent's app events, the app's state, a declared `$session` param. |
| `snapshot()`, `start(resume=...)` | Where the agent's conversation is, to pick it up in a later session: a `vasdk.ConversationSnapshot` (store it with `SerializeToString()`; `start` takes the message, its bytes or its JSON). |
| `vasdk.set_log_level("warn")` | How much VASDK (and the providers) log to the console. |

Everything else a C++ session does is here too:

| | |
| --- | --- |
| `reset_conversation`, `push_to_talk`, `set_follow_up_listen_ms`, `set_entity_values`, `set_backchannel`, `enroll_speaker`, `add_speaker` | Commands, as `send()` takes them. |
| `describe`, `using_fallbacks`, `next_scheduled_trigger`, `trigger_state`, `restore_trigger_state`, `speakers` | What the session runs, its triggers across restarts, and the enrolled voice prints to keep. |
| `on_context(fn)` | The app's state, asked each time the agent or the LLM reads it. |
| `add_processor(slot, fn)` | At `vasdk.AFTER_INPUT` (the microphone's audio), `BEFORE_OUTPUT` (the reply's, changed by returning new bytes), `AFTER_TRANSCRIPTION` and `BEFORE_SYNTHESIS` (text: return new text, or `""` to drop it). |
| `vasdk.Runtime(config, providers, max_sessions=4).session(agent, id=...)` | Many sessions over one set of loaded providers. |

Providers can be written in Python, and an engine config names them by id like built-in ones:

```python
providers = vasdk.Providers(built_in=False)   # only Python's; True also adds the C library's built-in ones

@providers.vad("loudness")
class Loudness:
    def __init__(self, sink, sample_rate): pass
    def detect(self, pcm): return 0.9 if max(abs(s) for s in memoryview(pcm).cast("h")) > 1000 else 0.0

@providers.llm("mine")
class Mine:
    def __init__(self, sink, sample_rate): pass
    def complete(self, messages): return "Sure."   # messages: [{"role", "content"}]
# engine.txtpb: providers { vad { id: "loudness" } llm { id: "mine" } ... }
```

`vad`, `stt`, `tts`, `turn`, `wake_word`, `audio_dsp`, `speaker_embedding`, `sentence_encoder`, `entity_model` and `llm`, each with the methods `Providers`' docstring lists; an STT and a TTS give their results through `sink` (`sink.text`, `sink.audio`, `sink.done`). A text `Conversation` has `snapshot`/`resume`, `set_entity_values`, `set_context`, `check_triggers`, `deliver_queued` and the trigger state; a `Turn` has `set_text`, `render`, `reply_text`, `flow`, `page` and `ask`; `vasdk.run_test(agent, case)` plays a `FlowTestCase` from memory. [`tests/test_providers.py`](tests/test_providers.py) uses all of it.

The binding's tests run with `-DVASDK_BUILD_PYTHON_BINDING=ON` (off by default).

[VASDK_PYTHON_APP](../../VASDK_PYTHON_APP/) is a whole app built this way: a kitchen assistant with a microphone, a scripted demo, and tests.

## Models

The model store downloads the model files local providers load, checks their SHA-256, and keeps them where a session looks when it gets no `models_dir` ([Models](../../docs/models.md)):

```python
models = vasdk.Models()                                   # directory=, manifest_path=, token=, offline=
for model_id in models.for_config("engine.txtpb"):        # what the config's providers load
    models.pull(model_id, progress=lambda model, file, done, total: print(f"{model}: {done * 100 // total}%"))
session = vasdk.Session("engine.txtpb", providers, agent)   # no models_dir: the store's directory
```

| Call | What |
| --- | --- |
| `Models.list()` | Every model of this machine: `id`, `license`, `size`, `status` (`present`, `partial`, `missing`)... |
| `Models.pull(model_id, accept_license=False, progress=None)` | Downloads what is missing, resuming; `progress` returning `False` stops it. `accept_license` for a non-commercial or custom license |
| `Models.verify`, `Models.remove`, `Models.status`, `Models.default_directory()` | |

`vasdk.api_version()` gives the library's `VASDK_C_API_VERSION`. Test: [`tests/test_models.py`](tests/test_models.py) (`vasdk_python_models`).

## Messages

The build generates Python classes from VASDK's `.proto` files ([VASDK/proto](../../VASDK/proto/)) into `build/bindings/python/vasdk` (the `vasdk_python_protos` target), with the same `protoc` that builds the C++ ones. They are subpackages of `vasdk`: `vasdk.engine.events_pb2`, `vasdk.flow.conversation_pb2`, `vasdk.flow.agent_pb2`, ... The ones you meet most are named on the package:

| | |
| --- | --- |
| `vasdk.Event` | Each session event ([events.proto](../../VASDK/proto/vasdk/engine/events.proto)): `event.WhichOneof("kind")`, then `event.agent_reply.text`, `event.tool_call.name`, ... |
| `vasdk.Command` | What `Session.send` takes. |
| `vasdk.ConversationReply`, `vasdk.ConversationInput` | A text conversation's turn and its reply ([conversation.proto](../../VASDK/proto/vasdk/flow/conversation.proto)); `Conversation.send(input)` takes any input. |
| `vasdk.StepOutcome` | What `turn.apply` takes (a dict of the same shape works too). |
| `vasdk.SessionOptions`, `vasdk.ToolDeclaration`, `vasdk.ToolParameter` | `Session(options=...)`, and the parameters `Session.tool` takes (dicts work too). |
| `vasdk.ConversationSnapshot` | `Session.snapshot()`: where an agent's conversation is, to resume it. |
| `vasdk.Language` | A language pack ([language.proto](../../VASDK/proto/vasdk/nlu/language.proto)): `Registry.add_language` takes one, `vasdk.builtin_language()` returns VASDK's en-US. See [an agent in another language](../../docs/examples/languages.md). |

A step's or page type's params arrive as their message when Python knows its class: VASDK's and Google's types, a class you pass as `message=`, one whose `_pb2` module you've imported, or one the agent's own `.proto` files declare (`proto_imports`: loading the agent adds them to Python's descriptor pool, from `vasdk_agent_proto_files`). Params of a type Python can't find arrive as a dict.

```python
# agent.txtpb: proto_imports: ["loyalty.proto"]   (package acme; message Bonus { string reason = 1; int32 points = 2; })
#              routes { intent: "bonus" steps { custom { [type.googleapis.com/acme.Bonus] { reason: "birthday" points: 50 } } } }
registry = vasdk.Registry()

@registry.step("acme.Bonus")
def bonus(params, turn):                   # an acme.Bonus message, though no _pb2 module was generated for it
    turn.say(f"{params.points} points for your {params.reason}.")

agent = vasdk.Agent.load("agent.txtpb", registry)
```

A session's conversation picks up where it was, in a later session:

```python
saved = session.snapshot().SerializeToString()   # a vasdk.ConversationSnapshot: store the bytes
...
session = vasdk.Session(config, providers, agent)
session.start(resume=saved)                      # the message, its bytes or its JSON
```

Values stay Python values: `turn.lookup` and `conversation.lookup` return them, and `set_param`, `fire_trigger` and `set_context` take them (they cross as `google.protobuf.Value` and `Struct`; a whole number comes back as an `int`). Tools and entity extractors take and give dicts, because their data is JSON: the LLM writes a tool's arguments, and as a `Struct` the integer `10` would come back as `10.0`.

The generated code needs the `protobuf` package at the version of VASDK's `protoc` or newer (`protoc --version`: 29.x is `protobuf>=5.29`).

## A wheel

A wheel carries the C library, `vasdk_c`, for one platform: the C API and every provider built in. It works with any Python 3 there.

1. Build VASDK. Every build makes the C library (`build/cmake/c_library/`).
2. Bundle the library and the libraries beside it into the package:

   ```sh
   python bindings/python/bundle.py build/cmake/c_library/Release    # the folder with vasdk_c.dll or libvasdk_c.so
   ```

   This copies the C library and what it loads: ONNX Runtime, and, on Windows, any installed DLL it imports (protobuf, abseil, curl when they come from vcpkg). It copies the generated messages too (`engine/`, `flow/`, `goal/`, `nlu/`, `providers/`), and the wheel depends on `protobuf`. The models stay outside the wheel (`Session(models_dir=...)`).

3. Build and install the wheel, with uv or pip:

   ```sh
   uv build bindings/python --out-dir dist                 # dist/vasdk-1.0.0-py3-none-<platform>.whl and the sdist
   pip wheel bindings/python -w dist                       # the same wheel, with pip
   pip install dist/vasdk-*.whl                            # or: uv pip install dist/vasdk-*.whl
   ```

   [`pyproject.toml`](pyproject.toml) holds the metadata and the one dependency, `protobuf>=5.29.5`; `setup.py` only tags the wheel for its platform, since it carries a native library.

To remove the copied libraries from the source tree, run `python bindings/python/bundle.py --clean`.

## Without a wheel

To use the package from the source tree instead, install it editable (`uv pip install -e bindings/python` or `pip install -e bindings/python`), or put `bindings/python` on `sys.path` as the tests do, and set `VASDK_LIBRARY` to the C library. If it isn't set, the package looks in `./build/cmake/c_library` (then `./build/VASDK`, for a shared VASDK). The messages are found in `build/bindings/python/vasdk`, or where `VASDK_PYTHON_PROTOS` points.

```sh
VASDK_LIBRARY=build/cmake/c_library/Release/vasdk_c.dll python bindings/python/tests/test_garage.py VASDK_APP/agents/garage-menu
```

The tests in `tests/` run in ctest as `vasdk_python_garage` and `vasdk_python_extensions`; the kitchen app's as `vasdk_python_kitchen` and `vasdk_python_session` (spoken, with the speech models).
