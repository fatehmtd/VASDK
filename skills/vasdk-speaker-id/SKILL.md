---
name: vasdk-speaker-id
description: Speaker identification in VASDK - voice prints with pyannote (WeSpeaker ResNet34-LM), enrollment and matching in SpeakerIdentificationProcessor, and implementing a new SpeakerEmbeddingProvider. Use when working on who is speaking, voice enrollment, or adding a speaker-embedding model.
---

# Speaker identification

**What it does:** tells who is speaking. A `SpeakerEmbeddingProvider` turns one utterance into a fixed-size voice print; `SpeakerIdentificationProcessor` enrolls prints and matches new ones by cosine similarity. A provider only wraps the model.

| | |
| --- | --- |
| Interface | `VASDK/include/VASDK/SpeakerEmbeddingProvider.h`, [contract](../../docs/reference/providers.md#speakerembeddingprovider) |
| Processor | `VASDK/include/VASDK/SpeakerIdentificationProcessor.h`, [notes](../../docs/reference/processors.md#speakeridentificationprocessor) |
| Wiring | an app processor in the session's `AfterTranscription` slot ([adding your own processors](../../docs/reference/engine.md#adding-your-own-processors)); see `VASDK_APP/main_pipeline.cpp` |

## Built in

| Backend | Folder | Kind |
| --- | --- | --- |
| pyannote (WeSpeaker ResNet34-LM) | `VASDK_3P_IMPL/PyannoteSpeaker/` | local ONNX, 16 kHz mono, Kaldi fbank front end matched to torchaudio, on by default, `va::thirdparty::speaker` |

Speaker identification is not yet selectable in the engine config: there is no `providers.speaker` spec. Adding one is core work ([vasdk-core-component](../vasdk-core-component/SKILL.md)), not part of a new provider.

## Contract pitfalls

- `computeEmbedding(samples, numSamples)` takes mono float in [-1, 1] and returns an empty vector on failure.
- `getAudioConfig()` reports the format the implementation's `initialize()` set; reject what the model can't take.
- Prints from different models aren't comparable: enrollments must be redone when the model changes. Say so on its page.
- Voice prints are biometric data: never log them, and say on its page where they are stored.

## Adding one

1. The steps, files and code rules in [vasdk-provider](../vasdk-provider/SKILL.md) (rows 4-7 apply once the stage is in the catalog).
2. Template: `VASDK_3P_IMPL/PyannoteSpeaker/`, including its parity test of the feature front end against the Python reference.

## Tests

- Unit: like `pyannote_fbank_parity` (`VASDK_3P_IMPL/PyannoteSpeaker/tests/`): features match the reference, the same speaker's utterances score above the threshold and different speakers below it. Skip without the model.
- Integration: a session with `SpeakerIdentificationProcessor` in the `AfterTranscription` slot, two voices of a local TTS enrolled, then each recognized.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) and the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks hold, and its page states the equal-error rate it was measured at.
