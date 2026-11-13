---
name: vasdk-turn-detection
description: End-of-turn detection and turn-taking in VASDK - turn-1-mini, SmartTurn v3, turn-taking presets, pauses versus finished turns, barge-in, back-channels, follow-ups, and implementing a new TurnDetectionProvider. Use when the assistant cuts people off, answers too slowly, or when adding a turn model.
---

# Turn detection

**What it does:** decides when the user has finished (answer now) and when a turn starts, including barge-in over the assistant. `TurnDetectionProcessor` asks the provider about each frame and emits `TurnStateFrame`s. Without a provider, VAD silence ends the turn.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/TurnDetectionProvider.h`, [contract](../../docs/reference/providers.md#turndetectionprovider) |
| Processor | `VASDK/include/VASDK/TurnDetectionProcessor.h`, [notes](../../docs/reference/processors.md#turndetectionprocessor) |
| Config | `providers { turn { id options } }` and `turn_taking { preset voice_activity backchannel }`, [engine config](../../docs/reference/engine-config.md#turn-taking) |
| Related | [back-channels](../../docs/reference/processors.md#back-channels-speech-over-the-agent-that-isnt-an-interruption), [follow-ups](../../docs/reference/llm.md#follow-ups), `VASDK/include/VASDK/AgentStateProcessor.h` |

## Built in

| Id | Folder | Kind | Options |
| --- | --- | --- | --- |
| `turn-1-mini` | `VASDK_3P_IMPL/Turn1Mini/` | local ONNX, streaming, English, on by default and preferred, `va::thirdparty::turn` | `Turn1MiniOptions`: `model_path`, `pause_threshold`, `threshold` |
| `smartturn-v3` | `VASDK_3P_IMPL/SmartTurnV3/` | local ONNX, multilingual, on by default, `va::thirdparty::turn` | `SmartTurnOptions`: `model_path` |

## Using it

```textproto
providers { turn { id: "turn-1-mini" } }   # English; smartturn-v3 for other languages
turn_taking { preset: BALANCED }   # SNAPPY for phone calls, PATIENT for people who stop to think; then override single values
```

If it cuts people off, use `PATIENT` or raise the pause threshold. If it answers too slowly, use `SNAPPY` or let it end on a pause (`setEvaluateOnPause`). Back-channels ("mm-hmm") duck the voice instead of stopping it; set them in `backchannel`.

## Contract pitfalls

- `evaluate(frame, agentIsSpeaking)` is called for every frame. Keep it cheap, and return `NONE` most of the time.
- `agentIsSpeaking` is how you tell `INTERRUPT` (barge-in) from `NORMAL`.
- A `TURN_END` while the VAD is only `PAUSED`/`ENDING` (with `setEvaluateOnPause(true)`) ends the segment early, so it needs `pauseThreshold()` confidence (default 0.85): a pause is often a breath.
- A `TURN_START` on VAD `STARTING` is held until the VAD confirms speech.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md). Interface: `va::audio::TurnDetectionProvider`. Options: a new `<Name>Options`.
2. Template: `VASDK_3P_IMPL/SmartTurnV3/` for a model that re-reads the turn from the ring buffer (`va::thirdparty::turn`, ONNX, a golden-parity test of its audio features against the Python reference: `VASDK_3P_IMPL/SmartTurnV3/tests/gen_features_golden.py` and `VASDK_3P_IMPL/SmartTurnV3/tests/golden/`); `VASDK_3P_IMPL/Turn1Mini/` for a streaming model that carries state across every audio frame (a golden of its scores from the reference's streaming loop: `VASDK_3P_IMPL/Turn1Mini/tests/gen_scores_golden.py`).
3. Register it with `registry.add<va::audio::TurnDetectionProvider, providers::<Name>Options>("<id>", ...)`.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: `VASDK_3P_IMPL/<Name>/tests/`. Check feature extraction against goldens made by the reference implementation (as `smartturn_features_parity`), and the decision on a finished versus an unfinished utterance.
- Integration: `vasdk_vad_turn_end`, `vasdk_turn_policy`, `vasdk_backchannel`, `vasdk_follow_up_window` and `vasdk_turn_assembler` in `VASDK/tests/` must stay green with the new provider; `vasdk_offline_conversation` with it in the engine config.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold, and a latency note (ms per evaluation, on which machine) is on its page.
