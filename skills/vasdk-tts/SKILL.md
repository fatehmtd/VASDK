---
name: vasdk-tts
description: Text-to-speech in VASDK - choosing and configuring a voice (Phonon, PocketTTS, Supertonic, MOSS-TTS-Nano, Cartesia, Deepgram, Soniox, Gradium, Fish Audio), barge-in cancellation, voices per reply part, and implementing a new GenericVoiceSynthesisProvider or QueuedVoiceSynthesisProvider. Use when working on synthesis, voices, playback stalls, or adding a TTS backend.
---

# Text-to-speech

**What it does:** turns the assistant's reply into audio. `TTSFrameProcessor` sends text as it streams from the LLM or agent, receives audio chunks through the listener, emits `SynthesisFrame`s to the output, and reports `SynthesisStateFrame`s upstream (including `INTERRUPTED` on barge-in).

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/GenericVoiceSynthesisProvider.h`, [contract](../../docs/reference/providers.md#genericvoicesynthesisprovider) |
| Base for local voices | `VASDK/include/VASDK/QueuedVoiceSynthesisProvider.h` |
| Processor | `VASDK/include/VASDK/TTSFrameProcessor.h`, [notes](../../docs/reference/processors.md#ttsframeprocessor) |
| Config | `providers { tts { id options fallback } }`, `audio { output { ... } }`, [engine config](../../docs/reference/engine-config.md#providers) |

## Built in

| Id | Folder | Kind | Options |
| --- | --- | --- | --- |
| `gradium` | `VASDK_3P_IMPL/GradiumppVoiceSynthesis/` | cloud | `CloudOptions` (`voice_id`) |
| `soniox` | `VASDK_3P_IMPL/SonioxppVoiceSynthesis/` | cloud | `CloudOptions` |
| `deepgram` | `VASDK_3P_IMPL/DeepgramppVoiceSynthesis/` | cloud | `CloudOptions` |
| `cartesia` | `VASDK_3P_IMPL/CartesiappVoiceSynthesis/` | cloud, WebSocket | `CloudOptions` |
| `fishaudio` | `VASDK_3P_IMPL/FishaudioppVoiceSynthesis/` | cloud | `CloudOptions` |
| `supertonic` | `VASDK_3P_IMPL/SupertonicTTS/` | local ONNX, 31 languages, OpenRAIL model | `LocalTtsOptions` |
| `moss` | `VASDK_3P_IMPL/MossTTS/` | local ONNX | `LocalTtsOptions` |
| `phonon` | `VASDK_3P_IMPL/PhononTTS/` | local GGUF, on by default; default models CC BY-NC | `LocalTtsOptions` |
| `pocket` | `VASDK_3P_IMPL/PocketTTS/` | local ONNX, on by default | `LocalTtsOptions` |

Voices, models and licenses: [docs/provider-catalog.md](../../docs/provider-catalog.md).

## Using it

```textproto
providers {
  tts { id: "phonon" options { [type.googleapis.com/vasdk.providers.LocalTtsOptions] { voice: "Zoey" language: "en" } }
        fallback: "pocket" }
}
```

An agent can say part of a reply in another voice or at another pace (`say { voice speed }`, [flows](../../docs/reference/flows.md#rich-replies)), which reaches the provider as `synthesizeStyledSpeech(text, style)`. The engine config's `audio { tts_prebuffer_ms }` (default 250, 0: off; a voice's own `prebuffer_ms` overrides it) trades first-word latency for no stalls on a slow CPU, for every local voice; cloud voices ignore it.

## Contract pitfalls

- **`cancelSynthesis()` makes barge-in work.** It must not block, must stop audio promptly, and must still fire `onSynthesisCompleted()` (or `onSynthesisError()`) once cancelled. Without that the agent stays "speaking" and talks over the user.
- A reply sent right after a cancel must still be spoken: cancel drops only what was queued before it.
- Audio leaves in the pipeline's `AudioConfig` (rate, format, channels). Convert inside the provider.
- `supportsSsml()` defaults to `false`; return `true` only if `synthesizeSpeech()` accepts a `<speak>` document.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md). Options: `CloudOptions` for a cloud voice, `LocalTtsOptions` for a local one, or a new message if neither fits exactly.
2. **A cloud voice:** copy `VASDK_3P_IMPL/CartesiappVoiceSynthesis/` (already `va::thirdparty::tts`) and implement `GenericVoiceSynthesisProvider`.
3. **A local voice:** derive from `QueuedVoiceSynthesisProvider` and implement only `loadEngine()` and `synthesizeOne(text)`: pass audio to `handoffAudio(pcm, sampleRate)` and `return finishHandoff();`. Use `style()` for the voice and pace, and `onCancel()` / `cancelFlag()` to stop early. An engine that streams on its own thread calls `handoffAudio()` from that thread, as Phonon does, so it keeps the pre-buffer; `Delivery::Direct` and `deliverAudio()` skip the pre-buffer, and with no cushion a slow chunk is heard as a gap. Copy the structure of `VASDK_3P_IMPL/SupertonicTTS/` (model download, test), but write it in `va::thirdparty::tts`, with an options struct and no file-scope `static` tables: Supertonic predates those rules.
4. Register it with `registry.add<va::audio::GenericVoiceSynthesisProvider, ...>("<id>", ...)`.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: `VASDK_3P_IMPL/<Name>/tests/<name>_test.cpp`. Synthesize a sentence at the pipeline's 16 kHz int16, check non-silent audio of a sane length, cancel mid-sentence and check the completion still fires. Write a WAV for listening. Template: `VASDK_3P_IMPL/SupertonicTTS/tests/synth_test.cpp` (`supertonic_tts_synth`). Skipped (`GTEST_SKIP()`) without the model or key.
- Integration: `vasdk_tts_turn` and `vasdk_speech_style` (`VASDK/tests/`) for the processor contract, `vasdk_llm_stream_speech` for streamed replies, and `vasdk_offline_conversation` with the new id as the assistant voice.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold. Restrictive voice licenses (non-commercial, OpenRAIL) are called out on the provider's page and in `THIRD_PARTY_NOTICES.md`.
