# The C API

[`VASDK/c/vasdk.h`](../../VASDK/include/VASDK/c/vasdk.h) gives any language that can call C what C++ apps have for [agents](flows.md): load an agent, talk to it by text, write its steps, tools, envelopes, page types and entity extractors, and run its test cases. Data crosses as JSON or serialized protos ([`vasdk/flow/*.proto`](../../VASDK/proto/vasdk/flow/)), never as C++ types, so bindings (Python, Rust, Go, Swift) stay thin.

```c
#include <VASDK/c/vasdk.h>

static void create_booking(const char* args_json, vasdk_result* result, void* user) {
    vasdk_result_set(result, "{\"reference\": \"B-42\"}");     /* or vasdk_result_fail(result, "why") */
}

vasdk_registry* registry = vasdk_registry_create();
vasdk_registry_add_tool(registry, "create_booking", "Books a service", create_booking, NULL);
char* error = NULL;
vasdk_agent* agent = vasdk_agent_load("agents/garage/agent.txtpb", registry, &error);
vasdk_conversation* conversation = vasdk_conversation_create(agent);
char* reply = vasdk_conversation_start(conversation);   /* {"text": "Hi, ...", "page": "welcome", ...} */
vasdk_free(reply);
reply = vasdk_conversation_say(conversation, "book an oil change");
vasdk_free(reply);
vasdk_conversation_free(conversation);
vasdk_agent_free(agent);
vasdk_registry_free(registry);
```

| Part | Functions |
|---|---|
| Registry | `vasdk_registry_add_step` (for `custom` steps, by their params type; params arrive as JSON), `vasdk_registry_add_tool` (for `call_tool`; set or fail its `vasdk_result`), `vasdk_registry_add_envelope` (`before` returns nonzero when it answered the turn, `after` runs on the reply), `vasdk_registry_add_page_type` (for pages with `type: "<name>"`: `on_enter`, and `on_turn` returning nonzero when it answered; params as JSON), `vasdk_registry_add_entity_extractor` (for `code { extractor }` entities: gets `{"words", "text"}`, sets an array of `{"start", "length", "value", "spoken", "attributes"}`). |
| Turn, inside a callback | `vasdk_turn_text`, `vasdk_turn_lookup("$session.tier")` (JSON), `vasdk_turn_say`, `vasdk_turn_goto`, and `vasdk_turn_apply` with a [`StepOutcome`](../../VASDK/proto/vasdk/flow/step_outcome.proto): values to set and unset, texts to say, a target. A key it may not write (anything but `$session.<declared>`, `$page.locals.<name>`, `$form.<slot>`) fails it, and nothing applies. |
| Agents | `vasdk_agent_load(path)`, or `vasdk_agent_parse(data, length, format)` from text proto, JSON or binary. `vasdk_agent_proto_files(agent, &length)`: the agent's `proto_imports` as a serialized `FileDescriptorSet`, for a language that builds its message classes at run time. |
| Conversations | `vasdk_conversation_start`, `_say`, `_no_input`, `_select_choice`, `_fire_trigger`, `_set_param` (a JSON value), `_lookup`. Each turn returns the reply as JSON: `text`, `segments`, `choices`, `matched`, `intent`, `event`, `expects_reply`, `ends_session`, `to_llm` and `llm_profile` (a route handed the turn to an LLM, [`target: "LLM"`](flows.md#with-an-llm): answer it with yours, by that profile; `text` is the answer without one), `flow`, `page`, `tool_calls`, `triggers`. |
| Tests | `vasdk_run_test_file(agent, path, &report)`: a [`FlowTestCase`](flows.md#checking-and-testing-vasdk_agentc) file, 1 when it passed. |

**Protos instead of JSON.** Every function above that takes or gives JSON has a version that takes and gives the proto itself, in a `vasdk_format` of the caller's choice: `VASDK_FORMAT_BINARY` for a language with protobuf (Python, Kotlin, Go), `VASDK_FORMAT_TEXT` for a readable one. What they return is freed with `vasdk_free`, and its size comes back through a `size_t*`.

| Function | The proto |
|---|---|
| `vasdk_conversation_send(conversation, input, length, format, &reply_length, &error)` | Every turn: a [`ConversationInput`](../../VASDK/proto/vasdk/flow/conversation.proto) (`start`, `text`, `no_input`, `select_choice`, `fire_trigger`) in, the `ConversationReply` out, with the same fields as the JSON reply. |
| `vasdk_conversation_set_param_message`, `vasdk_conversation_lookup_message`, `vasdk_turn_lookup_message` | A `google.protobuf.Value`. |
| `vasdk_registry_add_step_message`, `vasdk_registry_add_page_type_message` | The step's or page type's params message, serialized in `format`. |

Tools and entity extractors take and give JSON in every form: an LLM writes a tool's arguments, and the extractor's matches carry the same kind of free-form values.

`vasdk_set_log_level("warn")` sets how much VASDK logs to the console (`trace` to `off`); the shared `vasdk_providers` library has its own, `vasdk_providers_set_log_level`.

Text VASDK returns as `char*` is the caller's: free it with `vasdk_free`. On failure a function sets its `char** error` (also freed with `vasdk_free`) and returns `NULL` or 0. Callbacks and their `user` pointer must outlive the agents loaded with the registry. A conversation is used from one thread at a time; registries and agents can be shared. [`vasdk_c_test.c`](../../VASDK/tests/vasdk_c_test.c) uses all of it from C, in `ctest`.

## Spoken sessions

A `vasdk_session` is a spoken conversation: the speech providers an [engine config](engine-config.md) names (VAD, STT, TTS, an LLM) and, optionally, an agent. The app brings the audio. It pushes the user's 16-bit mono PCM in with `vasdk_session_push_audio`, in pieces of any size, and gets the assistant's speech through its audio callback. That fits a phone line, a file, or the app's own audio device.

```c
vasdk_providers* providers = vasdk_providers_create();
vasdk_providers_add_all(providers);               /* ProviderCatalog: every provider built in */
vasdk_session* session = vasdk_session_create("engine.txtpb", providers, agent, "{\"id\": \"call-1\"}", &error);
vasdk_session_on_event(session, on_event, NULL);  /* each vasdk.engine.Event, as JSON */
vasdk_session_on_audio(session, play, NULL);      /* the assistant's speech, at vasdk_session_output_rate */
vasdk_session_start(session, &error);
vasdk_session_push_audio(session, pcm, samples);  /* as the user's audio comes in */
...
vasdk_session_free(session);
vasdk_providers_free(providers);
```

| Function | What it does |
|---|---|
| `vasdk_providers_create`, `vasdk_providers_add_all` | The providers a config can name. `vasdk_providers_add_all` is in ProviderCatalog (`VASDK_3P_IMPL`): link `vasdk_provider_catalog`, or load the shared `vasdk_providers` library, built beside a shared VASDK for code that loads it at run time (Python). C++ code adds its own through `vasdk_providers_registry` (`VASDK/c/vasdk_cpp.h`). |
| `vasdk_session_create(config_path, providers, agent, options_json, &error)` | The config file (`${ENV}` expanded) and an agent, or `NULL` for the LLM alone. Options: `id`, `models_dir`, `system_prompt` (`{context}` in it is `vasdk_session_set_context`'s), `format_instruction` (how the LLM's replies are formatted). The config's audio must be 16-bit mono (the default). |
| `vasdk_session_on_event`, `vasdk_session_on_audio` | Set before `vasdk_session_start`. Events come on the session's event thread (`{"agentReply": {"text": ..., "isFinal": true}}`, `pageChanged`, `agentTurn`, ...), speech on a pipeline thread. |
| `vasdk_session_push_audio`, `vasdk_session_inject_text`, `vasdk_session_say` | The user's audio, a user turn by text, words the assistant says. Thread-safe. |
| `vasdk_session_set_param`, `vasdk_session_set_network_available` | A declared agent session param (a JSON value); the network going down or back, which switches to the config's fallbacks. |
| `vasdk_session_add_tool(session, name, description, parameters_json, tool, user, &error)` | A tool the session's LLM may call, written in the app's language: `parameters_json` is `[{"name", "description", "type", "required"}]`, and the callback is a `vasdk_tool_fn` (`vasdk_result_set`, or `vasdk_result_fail` to tell the LLM it failed). Before `vasdk_session_start`. An agent's `call_tool` tools go in its registry. |
| `vasdk_session_fire_trigger(session, name, payload_json, &error)` | Fires the agent's `app_event` triggers with that name; the payload is `$trigger.payload`. Its delivery decides when it's said. |
| `vasdk_session_set_context(session, context_json, &error)` | The app's state, a JSON object: the agent's `$context.*` (and its `context_change` triggers), and `{context}` in the system prompt. Each top-level key replaces the one before. |
| `vasdk_session_send(session, command, length, format, &error)` | Does what a [`vasdk.engine.Command`](../../VASDK/proto/vasdk/engine/events.proto) asks: `inject_text`, `select_choice`, `fire_trigger`, `say`, `interrupt`, `set_session_param`, the commands [remote control](engine-config.md#remote-control) takes. |
| `vasdk_session_create_message`, `vasdk_session_on_event_message`, `vasdk_session_add_tool_message`, `vasdk_session_set_context_message` | The same as the JSON functions, with protos: a [`SessionOptions`](../../VASDK/proto/vasdk/engine/session.proto), each `Event` itself (a `vasdk_message_fn` gets its bytes), a `ToolDeclaration`, a `google.protobuf.Struct`. |
| `vasdk_session_snapshot`, `vasdk_session_resume` | Where the agent's conversation is, sensitive values left out, and, before `start` on a later session, picking it up there ([flows](flows.md)): the JSON form of a [`ConversationSnapshot`](../../VASDK/proto/vasdk/flow/snapshot.proto). `vasdk_session_snapshot_message` and `vasdk_session_resume_message` take and give the message itself. |
| `vasdk_session_input_rate`, `vasdk_session_output_rate` | Sample rates in Hz: the config's `audio`, 16000 by default. |

[`c_session_test.cpp`](../../VASDK/tests/c_session_test.cpp) drives a session through these calls, in `ctest`.

## From Python

[`bindings/python/vasdk`](../../bindings/python/vasdk/__init__.py) wraps the C API with `ctypes`, so it needs no compiler: point `VASDK_LIBRARY` at a shared build of VASDK (`build/VASDK/libVASDK.dylib`), and add `bindings/python` to `PYTHONPATH`. It uses the proto functions, with Python classes the build generates from the same `.proto` files: replies are `vasdk.ConversationReply` messages and events `vasdk.Event` ([Messages](../../bindings/python/README.md#messages)). It needs `pip install protobuf`.

```python
import vasdk

registry = vasdk.Registry()

@registry.tool("save_booking")
def save_booking(booking):
    return {"reference": booking["reference"], "status": "confirmed"}   # raising fails it: tool-error

@registry.step("garage.BookingReference")
def booking_reference(params, turn):
    turn.apply({"set": {params["into"]: "AG-" + turn.lookup("$form.phone")[-4:]}})

registry.envelope("thanks", before=lambda turn: turn.text == "thanks" and (turn.say("You're welcome.") or True))

# type: "quiz" pages: asks on entering, answers a right guess itself (else the page's routes get the turn)
registry.page_type("quiz", "google.protobuf.Struct",
                   on_enter=lambda params, turn: turn.say(params["question"]),
                   on_turn=lambda params, turn: turn.text == params["answer"] and (turn.say("Right!") or True))

@registry.entity_extractor("contacts")   # code { extractor: "contacts" }
def contacts(words, text):
    return [{"start": i, "value": "+15550100", "spoken": "Mom"} for i, word in enumerate(words) if word == "mom"]

agent = vasdk.Agent.load("VASDK_APP/agents/garage-menu/agent.txtpb", registry)
conversation = agent.conversation()
conversation.start().text                          # "Hi, this is Acme Garage. ..."
conversation.say("book a tire repair").page        # "booking"
passed, report = vasdk.run_test_file(agent, "VASDK_APP/agents/garage-menu/tests/book_service.txtpb")
```

Errors raise `vasdk.VasdkError` with VASDK's message. The garage agent's whole suite runs from Python in `ctest` (`vasdk_python_garage`, [`test_garage.py`](../../bindings/python/tests/test_garage.py)), with its C++ step, envelope and tool written again in Python; a page type and an entity extractor in Python run in `vasdk_python_extensions`.

**Spoken sessions from Python.** `vasdk.Providers()` loads the `vasdk_providers` library, and `vasdk.Session(config, providers, agent, ...)` wraps a `vasdk_session`: `on_event` (each `vasdk.Event`), `on_audio`, `tool` (LLM tools in Python), `push_audio`, `send` (a `vasdk.Command`) and its shortcuts (`inject_text`, `fire_trigger`, `set_param`, `select_choice`, `interrupt`), `set_context`, `snapshot`. [VASDK_PYTHON_APP](../apps/python.md) is a kitchen assistant built on it, with its spoken test in `ctest` (`vasdk_python_session`).

**A wheel.** `python bindings/python/bundle.py build/VASDK/Release` copies the shared library and the libraries beside it into the package, and `pip wheel bindings/python` makes `vasdk-1.0.0-py3-none-<platform>.whl`: any Python 3 on that platform, no `VASDK_LIBRARY` needed once installed ([bindings/python/README.md](../../bindings/python/README.md)).
