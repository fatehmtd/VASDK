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
input → trigger → [AfterInput] → VAD → turn → STT → [AfterTranscription]
  → agent state → turn assembly → LLM → tools → [BeforeSynthesis] → TTS → [BeforeOutput] → output
```

`describe()` returns the actual chain with each provider, and `start()` logs it:

```
[VAEngine] Started: TransportInputProcessor (LocalTransport) → TriggerProcessor → VADFrameProcessor (SileroVAD) → TurnDetectionProcessor (SmartTurnV3Provider) → TranscriptionFrameProcessor (WhistleTranscriptionProvider) → AgentStateProcessor → TurnAssembler → UserTextInput → LLMProcessor (OpenAITransport) → ToolDispatchProcessor → SpeechInput → EventTap → TTSFrameProcessor (PocketVoiceSynthesisProvider) → TransportOutputProcessor (LocalTransport)
```

- **LLM.** The LLM and tool stages are there when the config has an `llm` endpoint, or when `Options::llmTransport` is set (for example a llama.cpp model). Without either, the assistant still listens and speaks `say()` text, but nothing answers the user.
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
| `injectUserText(text)` | Sends `text` as a finished user turn, as if it had been spoken. Needs an LLM. |
| `interrupt()` | Stops the current reply, the same way a barge-in does. |
| `registerTool(tool)` | Makes a tool available to the LLM; call it before `start()`. |

`say`, `injectUserText` and `interrupt` are thread-safe: the frame is queued and flows through the pipeline like any other.

## Events

`addListener` gets everything the assistant hears, says and does, without writing a processor. Each event is a `vasdk.engine.Event` ([`events.proto`](../../VASDK/proto/vasdk/engine/events.proto)) holding one of:

| Event | When |
|---|---|
| `transcript` | The user said something (partial and final results), or `injectUserText` sent it. Back-channels ("mm-hmm") are marked. |
| `agent_reply` | Once per streamed piece of the reply; `text` is the whole reply so far, `is_final` marks the end, with `expects_reply` and `choices`. `say()` text arrives as one final piece. |
| `agent_state` | Every state change: `IDLE`, `LISTENING`, `THINKING`, `SPEAKING`, `INTERRUPTED`, with the transition that caused it. |
| `tool_call` | A tool the LLM called, with its arguments and result, when it returns. |
| `error` | A processor or transport reported a failure, e.g. the LLM endpoint is unreachable. |

```cpp
engine->addListener([](const vasdk::engine::Event& event) {
    if (event.has_transcript() && event.transcript().is_final()) fmt::print("you: {}\n", event.transcript().text());
    if (event.has_agent_reply() && event.agent_reply().is_final()) fmt::print("assistant: {}\n", event.agent_reply().text());
});
```

Listeners run in order on one engine thread, never a pipeline thread, so a slow listener can't stall the audio. Keep them quick anyway: if they fall more than 1024 events behind, the oldest are dropped with a warning. A listener that throws is logged and the others still run. `removeListener(id)` takes one off; listeners can be added before or after `start()`.

## Transports

`Options::transport` is any `va::transport::AudioTransport`: `LocalTransport` (mic and speaker) or `WebRtcTransport` (a browser). The engine sets its audio formats, frame size and ring buffer, starts it in `start()`, and stops it in `stop()`. Set `Options::sendAllFramesToTransport` when the other end shows the agent's state (the WebRTC demo page does); otherwise only speech audio is sent.

## Availability

`VAEngine` needs engine config support (`VASDK_ENABLE_ENGINE_CONFIG`, on by default on desktop). The Android app still wires its own pipeline.
