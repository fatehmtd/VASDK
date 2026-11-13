---
name: vasdk-vad
description: Voice activity detection in VASDK - choosing and tuning a VAD (Silero, Energy+ZCR) in an engine config, and implementing a new VADProvider. Use when working on speech/silence detection, VAD thresholds, VADFrameProcessor, or adding a VAD backend.
---

# Voice activity detection

**What it does:** marks where speech is in each audio block. `VADFrameProcessor` turns those per-block answers into `VADStateFrame`s (`QUIET`, `STARTING`, `ONGOING`, `PAUSED`, `RESUMING`, `ENDING`, `ENDED`) and owns all the timing: debounce, hesitation versus end of utterance, lookback. The turn detector and STT act on those frames.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/VADProvider.h`, [contract](../../docs/reference/providers.md#vadprovider) |
| Processor | `VASDK/include/VASDK/VADFrameProcessor.h`, [state machine](../../docs/reference/processors.md#vadframeprocessor) |
| Config | `providers { vad { id options fallback } }` and `turn_taking { voice_activity { ... } }`, [engine config](../../docs/reference/engine-config.md#turn-taking) |

## Built in

| Id | Folder | Kind | Options | Notes |
| --- | --- | --- | --- | --- |
| `silero` | `VASDK_3P_IMPL/SileroVAD/` | local ONNX, on by default | `SileroOptions`: `model_path`, `threshold` (0.75) | model from `VASDK_3P_IMPL/cmake/DownloadSileroVADModel.cmake` |
| (code only) | `VASDK_3P_IMPL/EnergyZCRVAD/` | energy + zero-crossing, no model | `EnergyZCRVADOptions` | embedded targets; not in the catalog |

## Using it

```textproto
providers { vad { id: "silero" options { [type.googleapis.com/vasdk.providers.SileroOptions] { threshold: 0.6 } } } }
turn_taking { preset: SNAPPY voice_activity { ... } }   # timing lives here, not in the provider
```

A lower `threshold` catches quiet speakers, and more noise. Timing (how long a pause must last to end speech) belongs to the processor and is set in `turn_taking` (presets `BALANCED`, `SNAPPY`, `PATIENT`).

## Contract pitfalls

- **A pure per-block classifier.** `detectVoiceActivity()` is `const` and returns one `VADResult{offset, length, confidence}` per speech region in samples, empty for silence. No timers, no debouncing, no turn logic: that belongs to `VADFrameProcessor`.
- `initialize(config, bufferSize)` must reject a sample rate or format it can't handle (return `false`), not resample silently into wrong offsets.
- Offsets and lengths are in samples, not bytes.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md). Interface: `va::audio::VADProvider`. Options message: a new `<Name>Options`, or reuse `SileroOptions` if it is exactly `model_path` + `threshold`.
2. Template: `VASDK_3P_IMPL/SileroVAD/` (ONNX session, model download, `SileroVADOptions`) or `VASDK_3P_IMPL/EnergyZCRVAD/` (no model, `EnergyZCRVADOptions`, with a model-free unit test on synthetic audio: `VASDK_3P_IMPL/EnergyZCRVAD/tests/energy_zcr_vad_test.cpp`).
3. Register it with `registry.add<va::audio::VADProvider, providers::<Name>Options>("<id>", ...)`.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: `VASDK_3P_IMPL/<Name>/tests/<name>_test.cpp`. Feed speech (synthesized by a local TTS, as `vasdk_offline_conversation` does) followed by silence, and check the regions found and the silence left alone, at 16 kHz and at one rate it must reject.
- Integration: `vasdk_vad_turn_end` (`VASDK/tests/vad_turn_end_test.cpp`) for how VAD states end a turn, and `vasdk_offline_conversation` with the new id in its engine config.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold.
