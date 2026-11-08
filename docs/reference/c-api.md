# The C API

*Reference for the C API. Headers: [`vasdk.h`](../../VASDK/include/VASDK/c/vasdk.h), [`vasdk_providers.h`](../../VASDK/include/VASDK/c/vasdk_providers.h), [`vasdk_c_export.h`](../../VASDK/include/VASDK/c/vasdk_c_export.h) and the C++ wrapper [`vasdk_cpp.h`](../../VASDK/include/VASDK/c/vasdk_cpp.h) - the source of truth if this page and the code ever disagree.*

The C API is what the C library, `vasdk_c` (`VASDK::c`), exports: one shared library with the C API and every provider of its build, and nothing else visible. Another project gets it from `find_package(VASDK)`, pkg-config or a package ([Using VASDK in your project](../integrating.md)).

[`VASDK/c/vasdk.h`](../../VASDK/include/VASDK/c/vasdk.h) gives any language that can call C what C++ apps have for [agents](flows.md): load an agent, talk to it by text, write its steps, tools, envelopes, page types and entity extractors, and run its test cases. Data crosses as JSON or serialized protos ([`vasdk/flow/*.proto`](../../VASDK/proto/vasdk/flow/)), never as C++ types, so bindings (Python, Rust, Go, Swift) stay thin.

```c
#include <VASDK/c/vasdk.h>

static void create_booking(const char* args_json, vasdk_result* result, void* user) {
    vasdk_result_set(result, "{\"reference\": \"B-42\"}");     /* or vasdk_result_fail(result, "why") */
}

vasdk_registry* registry = vasdk_registry_create();
vasdk_registry_add_tool(registry, "create_booking", "Books a service", create_booking, NULL);
char* error = NULL;
vasdk_agent* agent = vasdk_agent_load("VASDK_APP/agents/garage-menu/agent.txtpb", registry, &error);
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
| Registry | `vasdk_registry_add_step` (for `custom` steps, by their params type; params arrive as JSON), `vasdk_registry_add_tool` (for `call_tool`; set or fail its `vasdk_result`), `vasdk_registry_add_envelope` (`before` returns nonzero when it answered the turn, `after` runs on the reply), `vasdk_registry_add_page_type` (for pages with `type: "<name>"`: `on_enter`, and `on_turn` returning nonzero when it answered; params as JSON), `vasdk_registry_add_entity_extractor` (for `code { extractor }` entities: gets `{"words", "text"}`, sets an array of `{"start", "length", "value", "spoken", "attributes"}`), `vasdk_registry_add_language` (a `vasdk.nlu.Language` pack agents may name in `locale` or `languages`; `vasdk_language_builtin` gives VASDK's en-US one: see [Languages](languages.md)). |
| Turn, inside a callback | `vasdk_turn_text`, `vasdk_turn_lookup("$session.tier")` (JSON), `vasdk_turn_say`, `vasdk_turn_goto`, `vasdk_turn_set_text` (what the rest of the turn reads as the user's words: an envelope redacts them), `vasdk_turn_render` (a template filled in, as a `say` does), `vasdk_turn_reply_text` (the reply so far), `vasdk_turn_flow` and `vasdk_turn_page`, and `vasdk_turn_apply` with a [`StepOutcome`](../../VASDK/proto/vasdk/flow/step_outcome.proto): values to set and unset, texts to say, a question to `ask` (with its choices), a target. A key it may not write (anything but `$session.<declared>`, `$page.locals.<name>`, `$form.<slot>`) fails it, and nothing applies. |
| Agents | `vasdk_agent_load(path)`, or `vasdk_agent_parse(data, length, format)` from text proto, JSON or binary. `vasdk_agent_proto_files(agent, &length)`: the agent's `proto_imports` as a serialized `FileDescriptorSet`, for a language that builds its message classes at run time. |
| Conversations | `vasdk_conversation_start`, `_say`, `_no_input`, `_select_choice`, `_fire_trigger`, `_set_param` (a JSON value), `_lookup`. Each turn returns the reply as JSON: `text`, `segments`, `choices`, `matched`, `intent`, `event`, `expects_reply`, `ends_session`, `to_llm` and `llm_profile` (a route handed the turn to an LLM, [`target: "LLM"`](flows.md#with-an-llm): answer it with yours, by that profile; `text` is the answer without one), `flow`, `page`, `tool_calls`, `triggers`. |
| Conversations, beyond turns | `vasdk_conversation_snapshot_message` and `_resume_message` (pick a conversation up later, in another conversation; `resumed` is 0 when the snapshot no longer fits the agent), `_set_entity_values_message` (a dynamic entity's values: an `EntityValuesCommand`), `_set_context_message` (what `$context.*` reads), `_reply_interrupted` (the user cut the reply off), `_no_input_ms` (how long to wait before a `no_input`), `_next_scheduled_trigger_ms`, `_trigger_state` and `_restore_trigger_state` (reminders across restarts). `check_triggers` and `deliver_queued` are `ConversationInput`s. |
| Tests | `vasdk_run_test_file(agent, path, &report)`: a [`FlowTestCase`](flows.md#checking-and-testing-vasdk_agentc) file, 1 when it passed. `vasdk_run_test_message`: the same, from memory. |

**Protos instead of JSON.** Every function above that takes or gives JSON has a version that takes and gives the proto itself, in a `vasdk_format` of the caller's choice: `VASDK_FORMAT_BINARY` for a language with protobuf (Python, Kotlin, Go), `VASDK_FORMAT_TEXT` for a readable one. What they return is freed with `vasdk_free`, and its size comes back through a `size_t*`.

| Function | The proto |
|---|---|
| `vasdk_conversation_send(conversation, input, length, format, &reply_length, &error)` | Every turn: a [`ConversationInput`](../../VASDK/proto/vasdk/flow/conversation.proto) (`start`, `text`, `no_input`, `select_choice`, `fire_trigger`, `check_triggers`, `deliver_queued`) in, the `ConversationReply` out, with the same fields as the JSON reply. |
| `vasdk_conversation_set_param_message`, `vasdk_conversation_lookup_message`, `vasdk_turn_lookup_message` | A `google.protobuf.Value`. |
| `vasdk_registry_add_step_message`, `vasdk_registry_add_page_type_message` | The step's or page type's params message, serialized in `format`. |

Tools and entity extractors take and give JSON in every form: an LLM writes a tool's arguments, and the extractor's matches carry the same kind of free-form values.

`vasdk_set_log_level("warn")` sets how much VASDK logs to the console (`trace` to `off`); the shared `vasdk_providers` library has its own, `vasdk_providers_set_log_level`.

Text VASDK returns as `char*` is the caller's: free it with `vasdk_free`. On failure a function sets its `char** error` (also freed with `vasdk_free`) and returns `NULL` or 0. Callbacks and their `user` pointer must outlive the agents loaded with the registry. A conversation is used from one thread at a time; registries and agents can be shared. [`vasdk_c_test.c`](../../VASDK/tests/vasdk_c_test.c) uses all of it from C, in `ctest`.

## Spoken sessions

A `vasdk_session` is a spoken conversation: the speech providers an [engine config](engine-config.md) names (VAD, STT, TTS, an LLM) and, optionally, an agent. The app brings the audio. It pushes the user's 16-bit mono PCM in with `vasdk_session_push_audio`, in pieces of any size, and gets the assistant's speech through its audio callback. That fits a phone line, a file, or the app's own audio device.

```c
#include <VASDK/c/vasdk_providers.h>   /* vasdk_providers_add_all; includes VASDK/c/vasdk.h */

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
| `vasdk_providers_create`, `vasdk_providers_add_all` | The providers a config can name. `vasdk_providers_add_all` is in ProviderCatalog (`VASDK_3P_IMPL`, declared in `ProviderCatalog/ProviderCatalogC.h`): link `vasdk_provider_catalog`, or load the shared `vasdk_providers` library, built beside a shared VASDK for code that loads it at run time (Python). C++ code adds its own through `vasdk_providers_registry` (`VASDK/c/vasdk_cpp.h`). |
| `vasdk_session_create(config_path, providers, agent, options_json, &error)` | The config file (`${ENV}` expanded) and an agent, or `NULL` for the LLM alone. Options: `id`, `models_dir`, `system_prompt` (`{context}` in it is `vasdk_session_set_context`'s), `format_instruction` (how the LLM's replies are formatted). The config's audio must be 16-bit mono (the default). |
| `vasdk_session_on_event`, `vasdk_session_on_audio` | Set before `vasdk_session_start`. Events come on the session's event thread (`{"agentReply": {"text": ..., "isFinal": true}}`, `pageChanged`, `agentTurn`, ...), speech on a pipeline thread. |
| `vasdk_session_push_audio`, `vasdk_session_inject_text`, `vasdk_session_say` | The user's audio, a user turn by text, words the assistant says. Thread-safe. |
| `vasdk_session_set_param`, `vasdk_session_set_network_available` | A declared agent session param (a JSON value); the network going down or back, which switches to the config's fallbacks. |
| `vasdk_session_add_tool(session, name, description, parameters_json, tool, user, &error)` | A tool the session's LLM may call, written in the app's language: `parameters_json` is `[{"name", "description", "type", "required"}]`, and the callback is a `vasdk_tool_fn` (`vasdk_result_set`, or `vasdk_result_fail` to tell the LLM it failed). Before `vasdk_session_start`. An agent's `call_tool` tools go in its registry. |
| `vasdk_session_fire_trigger(session, name, payload_json, &error)` | Fires the agent's `app_event` triggers with that name; the payload is `$trigger.payload`. Its delivery decides when it's said. |
| `vasdk_session_set_context(session, context_json, &error)` | The app's state, a JSON object: the agent's `$context.*` (and its `context_change` triggers), and `{context}` in the system prompt. Each top-level key replaces the one before. |
| `vasdk_session_send(session, command, length, format, &error)` | Does what a [`vasdk.engine.Command`](../../VASDK/proto/vasdk/engine/events.proto) asks: `inject_text`, `select_choice`, `fire_trigger`, `say`, `interrupt`, `set_session_param`, the commands [remote control](engine-config.md#remote-control) takes. |
| `vasdk_session_create_config`, `vasdk_runtime_create_config` | A session or runtime from an engine config the app built instead of a file: a [`vasdk.engine.EngineConfig`](../../VASDK/proto/vasdk/engine/engine_config.proto) in a `vasdk_format`, its providers' options their own messages ([options.proto](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto)). Nothing in it is expanded (`${ENV}`); errors name it "config". Python's `Session` and `Runtime` and Kotlin's take a `vasdk.EngineConfig` / `EngineConfig` the same way. |
| `vasdk_session_create_message`, `vasdk_session_on_event_message`, `vasdk_session_add_tool_message`, `vasdk_session_set_context_message` | The same as the JSON functions, with protos: a [`SessionOptions`](../../VASDK/proto/vasdk/engine/session.proto), each `Event` itself (a `vasdk_message_fn` gets its bytes), a `ToolDeclaration`, a `google.protobuf.Struct`. |
| `vasdk_session_snapshot`, `vasdk_session_resume` | Where the agent's conversation is, sensitive values left out, and, before `start` on a later session, picking it up there ([flows](flows.md)): the JSON form of a [`ConversationSnapshot`](../../VASDK/proto/vasdk/flow/snapshot.proto). `vasdk_session_snapshot_message` and `vasdk_session_resume_message` take and give the message itself. |
| `vasdk_session_input_rate`, `vasdk_session_output_rate` | Sample rates in Hz: the config's `audio`, 16000 by default. |

[`c_session_test.cpp`](../../VASDK/tests/c_session_test.cpp) drives a session through these calls, in `ctest`.

## Session controls, runtimes and processors

Everything a C++ `Session` does, a C session does too ([binding parity](../../skills/vasdk-binding-parity/parity.md) lists each call):

| Part | Functions |
|---|---|
| Commands | `vasdk_session_send` with a [`vasdk.engine.Command`](../../VASDK/proto/vasdk/engine/events.proto): `inject_text`, `say`, `select_choice`, `interrupt`, `fire_trigger`, `set_session_param`, `reset_conversation`, `push_to_talk`, `follow_up_listen_ms`, `set_entity_values`, `set_backchannel` (the fields set replace the session's), `enroll_speaker` (the next utterance enrolls that speaker; needs `providers.speaker_embedding`), `add_speaker` (restores a kept voice print). |
| Queries | `vasdk_session_describe`, `_using_fallbacks`, `_next_scheduled_trigger_ms` (for an alarm that wakes the app), `_trigger_state` and `_restore_trigger_state`, `_speakers_message` (the enrolled voice prints, a `vasdk.engine.VoicePrints`, to keep and give back with `add_speaker`). |
| Context | `vasdk_session_set_context` (state the app pushes) and `vasdk_session_on_context` (a callback asked each time the agent or the LLM reads it; its keys win). |
| Processors | `vasdk_session_add_audio_processor` at `VASDK_SLOT_AFTER_INPUT` (the microphone's audio, to read) or `VASDK_SLOT_BEFORE_OUTPUT` (the reply's audio, changed in place); `vasdk_session_add_text_processor` at `VASDK_SLOT_AFTER_TRANSCRIPTION` or `VASDK_SLOT_BEFORE_SYNTHESIS` (set new text, or `""` to drop it). Before `vasdk_session_start`. |
| Runtime | `vasdk_runtime_create(config, providers, models_dir, max_sessions)` (or `vasdk_runtime_create_config` with a config built in code), then `vasdk_runtime_session_create` per conversation: models load once per session running at the same time, as a call center needs. `vasdk_runtime_active_sessions`, `_idle_provider_sets`. |
| Device audio | No call: `providers { audio_input { id: "miniaudio" } audio_output { id: "miniaudio" } }` in the engine config ([`aaudio`](../providers/aaudio.md) on Android) and the session opens the microphone and speaker itself. |

## Providers written in the app

A VAD, an STT, a TTS, a turn model, a wake word, an audio DSP, a speaker embedding, a sentence encoder, an entity model or an LLM written in C (or in Python or Kotlin, which wrap these) is added to a `vasdk_providers` by id, and an engine config names it like a built-in one. Each session gets its own instance from `create`, and gives results back through the `vasdk_sink` it was made with: `vasdk_sink_text` (transcripts), `vasdk_sink_audio` and `vasdk_sink_done` (speech), `vasdk_sink_error`.

```c
static void* create(vasdk_sink* sink, int sample_rate, void* user) { return sink; }   /* the instance */
static float loudness(void* instance, const short* pcm, size_t samples) {
    int loudest = 0;
    for (size_t i = 0; i < samples; ++i) loudest = abs(pcm[i]) > loudest ? abs(pcm[i]) : loudest;
    return loudest > 1000 ? 0.9f : 0.0f;                                               /* 0.5 and above: speech */
}
static void complete(void* instance, const char* request_json, vasdk_result* reply) {
    vasdk_result_set(reply, "Sure.");                  /* request: {"messages": [{"role", "content"}]} */
}

vasdk_providers* providers = vasdk_providers_create();
vasdk_providers_add_vad(providers, "loudness", create, NULL, loudness, NULL);
vasdk_providers_add_llm(providers, "mine", create, NULL, complete, NULL, NULL);
/* engine.txtpb: providers { vad { id: "loudness" } llm { id: "mine" } ... } */
```

| Kind | What it implements |
|---|---|
| `vasdk_providers_add_vad` | the chance, 0 to 1, that the audio is speech |
| `vasdk_providers_add_stt` | `write` the audio, `end_of_speech`; with `streaming_transcription` off (the default) the final must come before `end_of_speech` returns |
| `vasdk_providers_add_tts` | `synthesize` text, its speech through `vasdk_sink_audio`, then `vasdk_sink_done`; `cancel` at a barge-in |
| `vasdk_providers_add_turn` | the chance the user is done, from their audio since they began |
| `vasdk_providers_add_wake_word` | each microphone frame; the phrase and its confidence when it's heard |
| `vasdk_providers_add_audio_dsp` | each microphone frame, processed into `output` (a copy of the input to start with) before anything hears it |
| `vasdk_providers_add_speaker_embedding` | one utterance's voice print (`vasdk_result_set_floats`, the same length each time); the session matches it to the enrolled speakers |
| `vasdk_providers_add_sentence_encoder` | a sentence's vector (`vasdk_result_set_floats`); one instance for every session of a runtime |
| `vasdk_providers_add_entity_model` | where entities are in the words, as JSON spans; one instance |
| `vasdk_providers_add_llm` | the reply to the messages, for the conversation and the agent's one-shot asks; `cancel` |

[`c_providers_test.cpp`](../../VASDK/tests/c_providers_test.cpp) runs a whole spoken session on providers and processors written in C, with no models; [`c_conversation_test.cpp`](../../VASDK/tests/c_conversation_test.cpp) covers the conversation and turn calls.

## From Python and Kotlin

[`bindings/python/vasdk`](../../bindings/python/vasdk/__init__.py) wraps the C API with `ctypes`, so it needs no compiler: point `VASDK_LIBRARY` at the C library (`build/cmake/c_library/libvasdk_c.dylib`; without it the build tree's is found), and add `bindings/python` to `PYTHONPATH`. It uses the proto functions, with Python classes the build generates from the same `.proto` files: replies are `vasdk.ConversationReply` messages and events `vasdk.Event` ([Messages](../../bindings/python/README.md#messages)). It needs `pip install protobuf`.

```python
import vasdk

registry = vasdk.Registry()

@registry.tool("save_booking")
def save_booking(booking):
    return {"reference": booking["reference"], "status": "confirmed"}   # raising fails it: tool-error

@registry.step("garage.BookingReference")   # params: a garage.BookingReference message
def booking_reference(params, turn):
    # "AG-" and four digits from the booking (FNV-1a), as the C++ step in GarageExtensions.cpp makes it
    booking = "".join(f"{turn.lookup(slot) or ''}|" for slot in ("$form.service", "$form.day", "$form.time", "$form.phone"))
    value = 2166136261
    for byte in booking.encode("utf-8"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    turn.apply({"set": {params.into: f"AG-{1000 + value % 9000}"}})

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

**Spoken sessions from Python.** `vasdk.Providers()` adds the providers the C library has built in, and `vasdk.Session(config, providers, agent, ...)` wraps a `vasdk_session`: `on_event` (each `vasdk.Event`), `on_audio`, `tool` (LLM tools in Python), `push_audio`, `send` (a `vasdk.Command`) and its shortcuts (`inject_text`, `fire_trigger`, `set_param`, `select_choice`, `interrupt`), `set_context`, `snapshot`. [VASDK_PYTHON_APP](../apps/python.md) is a kitchen assistant built on it, with its spoken test in `ctest` (`vasdk_python_session`).

**A wheel.** `python bindings/python/bundle.py build/cmake/c_library/Release` copies the C library and the libraries beside it into the package, and `pip wheel bindings/python` makes `vasdk-1.0.0-py3-none-<platform>.whl`: any Python 3 on that platform, no `VASDK_LIBRARY` needed once installed ([bindings/python/README.md](../../bindings/python/README.md)).

## Stability

The C API is VASDK's stable binary interface, the one the C library exports and every binding and package is built on:

- **Within a major version it only grows.** A release adds functions, enum values and fields at the end of a struct a caller doesn't allocate. It never changes or removes a function, and never changes a type's layout. A program built against 1.2 runs with any later 1.x library.
- **`VASDK_C_API_VERSION`** in `vasdk.h` counts the breaking changes, and is the library's soname: `libvasdk_c.so.1`, `libvasdk_c.1.dylib`. It rises only with a new major version. `vasdk_version()` gives the release (`VASDK_VERSION_STRING` in `<VASDK/version.h>` at compile time).
- **Only `vasdk_*` is exported.** `VASDK_C_API` (`vasdk_c_export.h`) marks the functions. On Linux and Android a linker version script hides everything else, and on macOS an exported symbols list does. On Windows only marked functions are exported. The static libraries inside (protobuf, abseil, spdlog, the providers) stay private, so they never clash with another copy in the program.
- **`vasdk_cpp.h` is not part of it.** It hands out C++ types and holds only for code built from the same source.

To add to the C API: add a function, never change one; give it `VASDK_C_API`; and bring the bindings level ([vasdk-bindings](../../skills/vasdk-bindings/SKILL.md)).
