---
name: vasdk-stt
description: Speech-to-text in VASDK - choosing and configuring an STT provider (Whistle, Parakeet, Moonshine, Deepgram, Soniox, Cartesia, Gradium, Fish Audio, Gladia, Voz), streaming versus batch transcription, and implementing a new GenericTranscriptionProvider. Use when working on transcription, partial results, recognition accuracy, or adding an STT backend.
---

# Speech-to-text

**What it does:** turns the user's speech into text. `TranscriptionFrameProcessor` feeds the provider audio from the ring buffer and emits `TranscriptionFrame`s (partial and final) that the agent and the LLM act on.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/GenericTranscriptionProvider.h`, [contract](../../docs/reference/providers.md#generictranscriptionprovider) |
| Processor | `VASDK/include/VASDK/TranscriptionProcessor.h`, [notes](../../docs/reference/processors.md#transcriptionframeprocessor) |
| Config | `providers { stt { id options fallback } }`, `turn_taking { streaming_transcription }`, [engine config](../../docs/reference/engine-config.md#providers) |

## Built in

| Id | Folder | Kind | Options |
| --- | --- | --- | --- |
| `gradium` | `VASDK_3P_IMPL/GradiumppTranscription/` | cloud, streaming | `CloudOptions` |
| `soniox` | `VASDK_3P_IMPL/SonioxppTranscription/` | cloud, streaming | `CloudOptions` |
| `deepgram` | `VASDK_3P_IMPL/DeepgramppTranscription/` | cloud, streaming | `CloudOptions` |
| `cartesia` | `VASDK_3P_IMPL/CartesiappTranscription/` | cloud, streaming | `CloudOptions` |
| `fishaudio` | `VASDK_3P_IMPL/FishaudioppTranscription/` | cloud, REST batch | `CloudOptions` |
| `gladia` | `VASDK_3P_IMPL/GladiappTranscription/` | cloud (WIP) | `CloudOptions` |
| `whistle` | `VASDK_3P_IMPL/WhistleSTT/` | local, on by default; favours the words the agent expects | `LocalSttOptions` |
| `moonshine` | `VASDK_3P_IMPL/MoonshineSTT/` | local ONNX, streaming and batch | `LocalSttOptions` |
| `parakeet` | `VASDK_3P_IMPL/Parakeet/` | local ONNX, batch | `LocalSttOptions` |
| (code only) | `VASDK_3P_IMPL/WhisperCpp/` | local GGUF (WIP) | constructor |
| (Android) | `VASDK_3P_IMPL/VozSTT/` | on-device, through JNI | Android app |

Per-provider notes, models and licenses: [docs/provider-catalog.md](../../docs/provider-catalog.md).

## Using it

```textproto
providers {
  stt { id: "deepgram" options { [type.googleapis.com/vasdk.providers.CloudOptions] { api_key: "${DEEPGRAM_API_KEY}" } }
        fallback: "whistle" }   # used when the cloud one isn't built, fails to start, or the network drops
}
turn_taking { streaming_transcription: true }   # partials while the user speaks; needed for speculative LLM firing
```

A recognizer's `language` must match the agent's `locale` ([languages](../../docs/reference/languages.md)). When the network drops, the session switches to the fallback ([engine](../../docs/reference/engine.md#when-the-network-drops)).

## Contract pitfalls

- **Two feeding modes.** Streaming: `writeAudioData()` with partials and finals through the listener. Batch: `writeFullAudioData()` gets one VAD-cut utterance. `writeFullAudioData()` throws by default, so a streaming-only provider needs the processor in streaming mode.
- **`commitEndOfSpeech()`**: finalize now when the VAD says the utterance ended; don't wait for an idle timeout.
- **`clearAudioBuffer()`** on barge-in: drop audio in progress, which may hold the assistant's own voice.
- **`setContextHints(phrases)`**: the words the agent expects next. Use them if the engine can bias towards them, as Whistle does.
- `languageTags` in the result, ordered by first appearance; several tags mean code-switching.
- Listener calls come from your thread; never block inside them. Never log transcripts at `info` (`VASDK/include/VASDK/Masking.h`).
- Cloud: honour `ca_bundle_path` (Android can't read the system store); report a dropped connection as an error, so the fallback takes over.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md). Options: `CloudOptions` for a cloud service, `LocalSttOptions` for a local model, or a new message if neither fits exactly.
2. Templates: cloud streaming over WebSocket: `VASDK_3P_IMPL/CartesiappTranscription/`; cloud REST: `VASDK_3P_IMPL/FishaudioppTranscription/`. Both follow the namespace rule. A local model: copy the structure of `VASDK_3P_IMPL/MoonshineSTT/` (streaming) or `VASDK_3P_IMPL/Parakeet/` (batch), in `va::thirdparty`, with an options struct.
3. Register it with `registry.add<va::audio::GenericTranscriptionProvider, ...>("<id>", ...)`, in the order of preference among the STT ids.

## Tests

- Unit: `VASDK_3P_IMPL/<Name>/tests/<name>_test.cpp`. Transcribe a known WAV and check the words (lower-cased, punctuation stripped); check partials arrive before the final when streaming; check `clearAudioBuffer()` drops audio. Template: `VASDK_3P_IMPL/MoonshineSTT/tests/stream_test.cpp` (`moonshine_stt_stream`). Exit 77 without the model or key.
- Integration: `vasdk_offline_conversation` with the new id; `vasdk_network_fallback` for a cloud one; accuracy on an agent's eval set with `vasdk_spoken_eval` and `garage_agentc eval` ([docs](../../docs/reference/flows.md#checking-and-testing-vasdk_agentc)).

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold.
