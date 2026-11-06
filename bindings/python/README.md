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
providers = vasdk.Providers()            # every VAD, STT and TTS provider in the vasdk_providers library
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

| | |
| --- | --- |
| `Providers()` | Loads the `vasdk_providers` library (`VASDK_PROVIDERS_LIBRARY`, else beside VASDK or in the build tree). It's built when VASDK is a shared library. |
| `Session(config, providers, agent, id, models_dir, system_prompt, format_instruction)` | Before `start()`: `on_event`, `on_audio`, `tool`. Callbacks run on VASDK's threads; keep them quick. |
| `push_audio`, `inject_text`, `say` | The user's audio, a user turn by text, words the assistant says. |
| `send(vasdk.Command(...))` | What a `Command` asks: `inject_text`, `select_choice`, `fire_trigger`, `say`, `interrupt`, `set_session_param`. `select_choice`, `interrupt` and the methods around them are shortcuts for it. |
| `fire_trigger`, `set_context`, `set_param` | The agent's app events, the app's state, a declared `$session` param. |
| `snapshot()`, `start(resume=...)` | Where the agent's conversation is, to pick it up in a later session: a `vasdk.ConversationSnapshot` (store it with `SerializeToString()`; `start` takes the message, its bytes or its JSON). |
| `vasdk.set_log_level("warn")` | How much VASDK (and the providers) log to the console. |

[VASDK_PYTHON_APP](../../VASDK_PYTHON_APP/) is a whole app built this way: a kitchen assistant with a microphone, a scripted demo, and tests.

## Messages

The build generates Python classes from VASDK's `.proto` files ([VASDK/proto](../../VASDK/proto/)) into `build/bindings/python/vasdk` (the `vasdk_python_protos` target), with the same `protoc` that builds the C++ ones. They are subpackages of `vasdk`: `vasdk.engine.events_pb2`, `vasdk.flow.conversation_pb2`, `vasdk.flow.agent_pb2`, ... The ones you meet most are named on the package:

| | |
| --- | --- |
| `vasdk.Event` | Each session event ([events.proto](../../VASDK/proto/vasdk/engine/events.proto)): `event.WhichOneof("kind")`, then `event.agent_reply.text`, `event.tool_call.name`, ... |
| `vasdk.Command` | What `Session.send` takes. |
| `vasdk.ConversationReply`, `vasdk.ConversationInput` | A text conversation's turn and its reply ([conversation.proto](../../VASDK/proto/vasdk/flow/conversation.proto)); `Conversation.send(input)` takes any input. |
| `vasdk.StepOutcome` | What `turn.apply` takes (a dict of the same shape works too). |
| `vasdk.SessionOptions`, `vasdk.ToolDeclaration`, `vasdk.ToolParameter` | `Session(options=...)`, and the parameters `Session.tool` takes (dicts work too). |

A step's or page type's params arrive as their message when Python knows its class: VASDK's and Google's types, a class you pass as `message=`, one whose `_pb2` module you've imported, or one the agent's own `.proto` files declare (`proto_imports`: loading the agent adds them to Python's descriptor pool, from `vasdk_agent_proto_files`). Params of a type Python can't find arrive as a dict.

Values stay Python values: `turn.lookup` and `conversation.lookup` return them, and `set_param`, `fire_trigger` and `set_context` take them (they cross as `google.protobuf.Value` and `Struct`; a whole number comes back as an `int`). Tools and entity extractors take and give dicts, because their data is JSON: the LLM writes a tool's arguments, and as a `Struct` the integer `10` would come back as `10.0`.

The generated code needs the `protobuf` package at the version of VASDK's `protoc` or newer (`protoc --version`: 29.x is `protobuf>=5.29`).

## A wheel

A wheel carries a shared build of VASDK for one platform. It works with any Python 3 there.

1. Build VASDK as a shared library. It is shared by default on Windows; elsewhere, configure with `-DBUILD_SHARED_LIBS=ON`.
2. Bundle the library and the libraries beside it into the package:

   ```sh
   python bindings/python/bundle.py build/VASDK/Release    # the folder with VASDK.dll or libVASDK.so
   ```

   On Windows this copies VASDK.dll and its dependencies: protobuf, abseil, curl, OpenSSL and zlib. It copies the generated messages too (`engine/`, `flow/`, `goal/`, `nlu/`), and the wheel depends on `protobuf`.

   For spoken sessions, add the speech providers' folder: `python bindings/python/bundle.py build/VASDK/Release build/VASDK_3P_IMPL/ProviderCatalog/Release`. That copies `vasdk_providers` and ONNX Runtime too; the models stay outside the wheel (`Session(models_dir=...)`).

3. Build and install the wheel:

   ```sh
   pip wheel bindings/python -w dist                       # dist/vasdk-1.0.0-py3-none-win_amd64.whl
   pip install dist/vasdk-*.whl
   ```

To remove the copied libraries from the source tree, run `python bindings/python/bundle.py --clean`.

## Without a wheel

To use the package from the source tree instead, set `VASDK_LIBRARY` to the shared library. If it isn't set, the package looks in `./build/VASDK`. The messages are found in `build/bindings/python/vasdk`, or where `VASDK_PYTHON_PROTOS` points.

```sh
VASDK_LIBRARY=build/VASDK/Release/VASDK.dll python bindings/python/tests/test_garage.py VASDK_APP/agents/garage-menu
```

The tests in `tests/` run in ctest as `vasdk_python_garage` and `vasdk_python_extensions`; the kitchen app's as `vasdk_python_kitchen` and `vasdk_python_session` (spoken, with the speech models).
