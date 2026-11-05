# VAEngine

Source of truth: [`VAEngine.h`](../../VASDK/include/VASDK/VAEngine.h), [`VAEngine.cpp`](../../VASDK/src/VAEngine.cpp).

`VAEngine` builds a complete voice assistant from an [engine config](engine-config.md). It creates the VAD, STT, TTS and turn-detection providers the config names, then wires the standard pipeline, so an app doesn't assemble processors by hand. You supply the audio transport and, optionally, your own processors and tools.

```cpp
#include <VASDK/VAEngine.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/engine/ProviderRegistry.h>
#include "ProviderCatalog/ProviderCatalog.h"   // link vasdk_provider_catalog

va::engine::ProviderRegistry registry;
va::thirdparty::registerAllProviders(registry);

va::VAEngine::Options options;
options.transport = std::make_shared<va::transport::LocalTransport>(micSource, speakerSink);
options.modelsDir = "models";
options.systemPrompt = "You are a helpful voice assistant. Keep answers short.";

std::string error;
auto engine = va::VAEngine::create("engine.txtpb", registry, options, &error);
if (!engine || !engine->start(&error)) { spdlog::error(error); return 1; }
engine->say("Hi, how can I help?");
// ... run until the user quits ...
engine->stop();
```

[`VASDK_APP/main_pipeline.cpp`](../../VASDK_APP/main_pipeline.cpp) is a full example: it adds audio I/O, demo tools, session recording, visualisers and a terminal UI around the engine.

## The pipeline

```
input → trigger → [wake word] → [AfterInput] → VAD → turn → STT → [AfterTranscription]
  → agent state → turn assembly → [dialog] → LLM → tools → [BeforeSynthesis] → TTS → [BeforeOutput] → output
```

`describe()` returns the actual chain with each provider, and `start()` logs it:

```
[VAEngine] Started: TransportInputProcessor (LocalTransport) → TriggerProcessor → VADFrameProcessor (SileroVAD) → TurnDetectionProcessor (SmartTurnV3Provider) → TranscriptionFrameProcessor (WhistleTranscriptionProvider) → AgentStateProcessor → TurnAssembler → UserTextInput → LLMProcessor (OpenAITransport) → ToolDispatchProcessor → SpeechInput → EventTap → TTSFrameProcessor (PocketVoiceSynthesisProvider) → TransportOutputProcessor (LocalTransport)
```

- **Agent.** The dialog stage is there when `Options::agent` is set: an agent file's pages answer each turn with no LLM, and a turn no route understands goes on to the LLM when there is one. See [Agents](flows.md).
- **LLM.** The LLM and tool stages are there when the config has an `llm` endpoint, or when `Options::llmTransport` is set (for example a llama.cpp model). With neither an agent nor an LLM, the assistant still listens and speaks `say()` text, but nothing answers the user.
- **Turn detection.** When no turn model is built in, the VAD's silence ends the turn.
- **Defaults.** VAD timing, STT settings and LLM sampling use the values VASDK_Pipeline has always used. Back-channel handling comes from `Options::backchannel`.

## Adding your own processors

`addProcessor(slot, processor)` puts a processor at one of four places, before `start()`:

| Slot | Sees | Typical use |
|---|---|---|
| `AfterInput` | raw mic audio, before VAD | recorders, mic visualisers |
| `AfterTranscription` | transcriptions, before the agent reacts | speaker identification, logging |
| `BeforeSynthesis` | reply text, before TTS | transcript recorders, text filters |
| `BeforeOutput` | synthesized audio, before playback | output visualisers |

Processors that read the mic audio directly use `inputRingBuffer()`.

## Controlling a running engine

| Call | Effect |
|---|---|
| `say(text)` | Speaks `text` as the assistant. |
| `injectUserText(text)` | Sends `text` as a finished user turn, as if it had been spoken. Needs an LLM or an agent. |
| `selectChoice(index)` | With an agent: taps the last reply's choice at `index` (from 0), the same as saying its value. |
| `interrupt()` | Stops the current reply, the same way a barge-in does. |
| `fireTrigger(name, payload)` | With an agent: fires one of its [triggers](flows.md#triggers), e.g. `"car-ready"` with `{"car": "Golf"}`. Its delivery decides when it's said. |
| `resetConversation()` | Starts over: cuts the reply, drops the LLM's history, and sends an agent back to its start page with its session params at their defaults. |
| `setBackchannel(config)`, `backchannel()` | Changes, or reads, how speech over the assistant is handled (back-channels, ducking) while it runs, e.g. from a settings screen. |
| `pushToTalk(held)` | The push-to-talk button: listens while held, and cuts a reply in progress. Released mid-sentence, the user's turn still gets its answer. |
| `setFollowUpListenMs(ms)` | How long the mic stays open after each reply; overrides the config's [`activation.follow_up`](engine-config.md#activation). An agent's own settings come first: see [Expected input](flows.md#expected-input). |
| `setSessionParam(name, value)` | With an agent: sets a declared `$session.<name>`, e.g. the caller's tier. `Options::context` gives the agent's `$context.*` a `ContextProvider`. See [Agents](flows.md#stored-values-and-conditions). |
| `setEntityValues(name, values)` | With an agent: replaces a `dynamic` entity's values for this conversation, e.g. the appointment times still free today. See [Entities](flows.md#entities). |
| `registerTool(tool)` | Makes a tool available to the LLM; call it before `start()`. |
| `setNetworkAvailable(up)`, `usingFallbacks()` | The network dropped, or came back: see [When the network drops](#when-the-network-drops). The Android app calls it from the system's connectivity changes. |

`say`, `injectUserText` and `interrupt` are thread-safe: the frame is queued and flows through the pipeline like any other.

## Events

`addListener` gets everything the assistant hears, says and does, without writing a processor. Each event is a `vasdk.engine.Event` ([`events.proto`](../../VASDK/proto/vasdk/engine/events.proto)) holding one of:

| Event | When |
|---|---|
| `transcript` | The user said something (partial and final results), or `injectUserText` sent it. Back-channels ("mm-hmm") are marked. |
| `agent_reply` | Once per streamed piece of the reply; `text` is the whole reply so far, `is_final` marks the end, with `expects_reply`, `choices` (with `choice_values`, what each sends back), `ui_json` (an agent's `ui`) and `expect` (an agent's [expected input](flows.md#expected-input): a keypad, a yes/no). `say()` text arrives as one final piece. |
| `agent_state` | Every state change: `IDLE`, `LISTENING`, `THINKING`, `SPEAKING`, `INTERRUPTED`, with the transition that caused it. |
| `tool_call` | A tool the LLM or an agent's `call_tool` called, with its arguments and result, when it returns. Sensitive values are masked. |
| `trigger_fired` | With an agent: a trigger fired, and its decision: `STARTED`, `QUEUED`, `DROPPED`, `EXPIRED` or `SUPPRESSED` (quiet hours), with the reason. |
| `user_speech` | The user's voice as voice activity detection hears it: `STARTED`, `ONGOING` (after a pause), `ENDED`. For a "you're speaking" indicator. |
| `error` | A processor or transport reported a failure, e.g. the LLM endpoint is unreachable. |
| `listening_window` | The mic stays open after a reply: `OPENED` (with `closes_in_ms`), `EXTENDED` when the user starts answering, `CLOSED` when it times out. For a live mic indicator. |
| `page_changed` | With an agent: the conversation moved to another page (`from_flow`/`from_page` → `to_flow`/`to_page`), with the reason: `start`, `restart`, `route:<intent>` or `goto`. |
| `agent_turn` | With an agent: it answered (a user turn, silence, a trigger, its greeting), before the words are spoken: the route's `intent`, `matched`, the `event` it raised (`no-match`, `no-input`, `max-reprompts`, `tool-error`, `clarify`) and the `flow` and `page` it's on. For per-call metrics. |
| `session_ended` | With an agent: an `end` step or `END_SESSION` ended the conversation; the next one starts over at the start page. |
| `provider_switched` | STT, TTS or the LLM changed because the network dropped or came back, or a provider failed: `kind` (`stt`, `tts`, `llm`), `from` → `to`, and the reason. |

```cpp
engine->addListener([](const vasdk::engine::Event& event) {
    if (event.has_transcript() && event.transcript().is_final()) fmt::print("you: {}\n", event.transcript().text());
    if (event.has_agent_reply() && event.agent_reply().is_final()) fmt::print("assistant: {}\n", event.agent_reply().text());
});
```

A browser or a script can watch the same events, and send commands, over HTTP: see [remote control](engine-config.md#remote-control).

Listeners run in order on one engine thread, never a pipeline thread, so a slow listener can't stall the audio. Keep them quick anyway: if they fall more than 1024 events behind, the oldest are dropped with a warning. A listener that throws is logged and the others still run. `removeListener(id)` takes one off; listeners can be added before or after `start()`.

## When the network drops

A conversation keeps going without the network when the config names local fallbacks:

```textproto
providers {
  stt { id: "soniox" fallback: ["parakeet"] }
  tts { id: "cartesia" fallback: ["pocket"] }
}
```

| What happens | Network down (`setNetworkAvailable(false)`) | A cloud STT or TTS reports an error |
|---|---|---|
| STT, TTS | Switch to the first fallback that starts. It's loaded in the background the first time, then kept with the session's providers. The switch waits for a turn boundary: no speech being heard, no reply playing. | The same |
| LLM | Not asked: an agent answers on its own (no LLM stage, `llm` steps run their `offline` steps), and turns it doesn't understand aren't passed on | Unchanged |
| Back | `setNetworkAvailable(true)`: the configured providers and the LLM again | Stays on the fallback until the app says the network is up |

An agent's LLM requests also rest for 30 seconds after one fails, so a turn doesn't wait out the timeout each time. `fallback_options { key: "parakeet" value { [type.googleapis.com/vasdk.providers.LocalSttOptions] { model_dir: "..." } } }` gives a fallback its own options. A reply the failure cut off is said again by the fallback voice, from its first part the user hadn't fully heard; not if the user spoke meanwhile or another reply came first. An intent check that's only slow (past `llm.intent_timeout_ms`) doesn't rest the LLM.

## Flow metrics

With an agent and a metrics collector, the dialog reports how conversations go:
- counters: `flow_turns`, `flow_no_match`, `flow_no_input`, `flow_max_reprompts`, `flow_tool_errors`, `flow_clarify`, `flow_page_changes`, `flow_sessions_ended`, `flow_passed_to_llm` and `flow_triggers_said`
- a sampler: `flow_turn_ms`, the time to answer a turn

`flow_passed_to_llm` against `flow_turns` is the share of turns the agent answered on its own.

## Listening for the answer

With an agent, every question tells the pipeline how to listen for its answer: a `TurnPolicyFrame` goes upstream with the reply.
- The VAD and the turn detector use its turn end: patient for a phone number said with pauses, snappy for a quick yes.
- The speech recognizer gets the words to expect, through `GenericTranscriptionProvider::setContextHints`. That is a no-op by default; Soniox implements it.

See [Expected input](flows.md#expected-input).

## Transports

`Options::transport` is any `va::transport::AudioTransport`: `LocalTransport` (mic and speaker) or `WebRtcTransport` (a browser). The engine sets its audio formats, frame size and ring buffer, starts it in `start()`, and stops it in `stop()`. Set `Options::sendAllFramesToTransport` when the other end shows the agent's state (the WebRTC demo page does); otherwise only speech audio is sent.

## Many conversations at once

`VAEngine` is one conversation with its own providers. A server handling many calls creates one `engine::Runtime` and one `engine::Session` per call ([`Runtime.h`](../../VASDK/include/VASDK/engine/Runtime.h), [`Session.h`](../../VASDK/include/VASDK/engine/Session.h)). A session has everything described above: the pipeline, slots, controls and events.

```cpp
va::engine::Runtime::Options limits;
limits.maxSessions = 10;
auto runtime = va::engine::Runtime::create("engine.txtpb", registry, limits, &error);

va::SessionManager calls(runtime);   // optional: sessions by id
va::engine::Session::Options call;
call.id = callId;                    // in logs and in every event's session_id
call.transport = transportForTheCall;
if (auto session = calls.createSession(call, &error)) session->start();
else rejectCall(error);              // all 10 sessions busy
// ... on hang-up:
calls.destroySession(callId);
```

- **Models load once per concurrent call.** The runtime keeps sets of providers (VAD, STT, TTS, turn model). A session takes one while it runs and hands it back when it ends, so the next call reuses it instead of loading the models again. The first set loads in `Runtime::create`, so a misconfigured provider fails there. Each further set loads the first time that many calls run at once.
- **Sessions are isolated.** Each has its own pipeline, mic ring buffer, LLM conversation and event thread. An LLM transport passed in `Options::llmTransport` holds the conversation, so give each session its own.
- **Limits.** `Runtime::Options::maxSessions` caps concurrent sessions; `createSession` returns `nullptr` with an error beyond it.
- `Session::toolDispatcher()` and `llmTransport()` give goal code what it needs to swap prompts and tools during a call.
- [`VASDK_CallCenter`](../../VASDK_CALLCENTER_APP/callcenter_pipeline.cpp) is a full example of a call: an agent per DNIS, the caller's number as a session param, and the call ending with the agent.

## Availability

`VAEngine` needs engine config support (`VASDK_ENABLE_ENGINE_CONFIG`, on by default everywhere, Android included). The Android app runs on it: [`VasdkEngine.kt`](../../VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/VasdkEngine.kt) builds the engine config from its settings as text proto, and [`vasdk_jni.cpp`](../../VASDK_ANDROID/app/src/main/cpp/vasdk_jni.cpp) creates a `Runtime` and a `Session` from it. Cross-compiling, code is generated with the build machine's `protoc` of protobuf's version (36.2), downloaded when there's none.
