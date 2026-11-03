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
  → agent state → LLM → tools → [BeforeSynthesis] → TTS → [BeforeOutput] → output
```

`describe()` returns the actual chain with each provider, and `start()` logs it:

```
[VAEngine] Started: TransportInputProcessor (LocalTransport) → TriggerProcessor → VADFrameProcessor (SileroVAD) → TurnDetectionProcessor (SmartTurnV3Provider) → TranscriptionFrameProcessor (WhistleTranscriptionProvider) → AgentStateProcessor → UserTextInput → LLMProcessor (OpenAITransport) → ToolDispatchProcessor → SpeechInput → TTSFrameProcessor (PocketVoiceSynthesisProvider) → TransportOutputProcessor (LocalTransport)
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

## Transports

`Options::transport` is any `va::transport::AudioTransport`: `LocalTransport` (mic and speaker) or `WebRtcTransport` (a browser). The engine sets its audio formats, frame size and ring buffer, starts it in `start()`, and stops it in `stop()`. Set `Options::sendAllFramesToTransport` when the other end shows the agent's state (the WebRTC demo page does); otherwise only speech audio is sent.

## Availability

`VAEngine` needs engine config support (`VASDK_ENABLE_ENGINE_CONFIG`, on by default on desktop). The Android app still wires its own pipeline.
