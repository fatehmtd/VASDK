---
name: vasdk-wake-word
description: Wake word and activation in VASDK - WAKE_WORD, PUSH_TO_TALK and HYBRID activation modes, the follow-up listening window, and implementing the first WakeWordProvider (none ships yet). Use when adding a wake phrase ("hey garage"), a keyword spotter, or changing when the assistant listens.
---

# Wake word

**What it does:** keeps the assistant asleep until a wake phrase. `WakeWordProcessor` feeds every mic frame, in order, to the provider; a detection wakes the session. The core, the config and the tests support it, but **no wake-word provider ships yet**: the first one is open work.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/WakeWordProvider.h` |
| Processor | `VASDK/include/VASDK/WakeWordProcessor.h` |
| Config | `activation { mode follow_up { listen_ms } }` and `providers { wake_word { id options } }`, [activation](../../docs/reference/engine-config.md#activation) |
| Example | `VASDK/tests/activation_test.cpp` registers a fake provider |

## Using it

```textproto
activation { mode: WAKE_WORD follow_up { listen_ms: 8000 } }
providers { wake_word { id: "<id>" } }
```

`HYBRID` takes the wake phrase or the push-to-talk button (`Session::pushToTalk`). After every reply the mic stays open for `follow_up.listen_ms`, so the user can answer without the phrase.

## Contract

```cpp
virtual bool initialize(const AudioConfig& audioConfig) = 0;
virtual std::optional<Detection> processAudio(const char* samples, size_t numSamples) = 0;  // {phrase, confidence}
virtual void reset() {}   // forget partial matches
```

- Called on every frame while asleep, so CPU per frame matters most: measure it on the slowest target (a phone).
- Keep streaming state between calls; report a detection once, not on every frame after it.
- Expose the sensitivity as an option. False accepts wake the assistant on TV audio; false rejects make users repeat themselves.
- Check the model's license before choosing one: many keyword-spotting models are non-commercial.

## Adding the first one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md): `va::thirdparty::wakeword`, `<Name>Options` (model path, phrases, sensitivity) with defaults, a download script for the model, every platform including Android.
2. Template: `VASDK_3P_IMPL/SmartTurnV3/` for the ONNX audio-model structure.
3. Register it with `registry.add<va::audio::WakeWordProvider, providers::<Name>Options>("<id>", ...)`. The stage is already in `VASDK/proto/vasdk/engine/engine_config.proto` (`wake_word`), and the runtime already uses it.
4. Docs: its page; in [activation](../../docs/reference/engine-config.md#activation), replace "No wake-word provider ships yet" with the new id; add a "Wake word" row to the README provider table and to `docs/provider-catalog.md`.

## Tests

- Unit: `VASDK_3P_IMPL/<Name>/tests/<name>_test.cpp`. Detects the phrase in a recording, stays quiet on speech without it and on silence, detects once per utterance, `reset()` clears partial matches.
- Integration: `vasdk_activation` and `vasdk_follow_up_window` (`VASDK/tests/`) with the real provider in place of the fake (a variant that skips without the model), then a spoken session through the catalog in the style of `vasdk_offline_conversation`.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold, and the page states CPU per second of audio and the false-accept rate it was measured at.
