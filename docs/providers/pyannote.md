# pyannote speaker embedding

*Speaker embedding provider, config id `pyannote` (`providers.speaker_embedding`), target `vasdk_pyannote_speaker`, build flag `-DVASDK_BUILD_PYANNOTE_SPEAKER=ON`. Source: [`VASDK_3P_IMPL/PyannoteSpeaker/`](../../VASDK_3P_IMPL/PyannoteSpeaker/). Interface: [`SpeakerEmbeddingProvider.h`](../../VASDK/include/VASDK/SpeakerEmbeddingProvider.h).*

The embedding model of pyannote/speaker-diarization-3.1 (WeSpeaker ResNet34-LM, ONNX) on a Kaldi fbank front end: 256-number voice prints for "same speaker or a new one" and enrolled names. On by default. It needs 16 kHz audio, the pipeline's default.

## Platforms

| macOS | Linux | Windows | Android |
| --- | --- | --- | --- |
| yes | yes | yes | yes |

## Options

In an engine config, `vasdk.providers.PyannoteOptions` (`model_path`; unset: `<models_dir>/pyannote_embedding.onnx`). In C++, `va::thirdparty::speaker::PyannoteEmbeddingOptions` (in [`PyannoteEmbeddingProvider.h`](../../VASDK_3P_IMPL/PyannoteSpeaker/PyannoteEmbeddingProvider.h)). Every field has a default except the ones marked "required" in its comment.

| Field | Default | Meaning |
| --- | --- | --- |
| `modelPath` | `""` | The embedding model, ONNX (VASDK_3P_IMPL/cmake/DownloadPyannoteEmbeddingModel.cmake). |

## Models and licenses

| Files | Source | License | Fetched by |
| --- | --- | --- | --- |
| pyannote_embedding.onnx | [wespeaker_en_voxceleb_resnet34_LM.onnx](https://github.com/k2-fsa/sherpa-onnx/releases/tag/speaker-recongition-models) (sherpa-onnx export) | CC BY 4.0 | `VASDK_3P_IMPL/cmake/DownloadPyannoteEmbeddingModel.cmake` |

Pipeline: [pyannote/speaker-diarization-3.1](https://huggingface.co/pyannote/speaker-diarization-3.1), MIT.

All of them are listed in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) and in [Providers, sources and licenses](../provider-catalog.md#sources-and-licenses). An app that ships this provider ships these licenses with it ([vasdk-app](../../skills/vasdk-app/SKILL.md#licenses-ship-with-the-app)).

## Usage

In an engine config, matched as [Speaker identification](../reference/engine-config.md#speaker-identification) says:

```textproto
providers { speaker_embedding { id: "pyannote" } }
speaker_identification { threshold: 0.3 min_speech_seconds: 1 }
```

The session then ends each utterance with a `speaker_identified` event. The `enroll_speaker` command (`Session::enrollSpeaker`, Python's `enroll_speaker`, Kotlin's `enrollSpeaker`) enrolls the next one. `speakers()` gives the voice prints to keep, and `add_speaker` restores them.

In a pipeline built by hand:

```cpp
#include "PyannoteSpeaker/PyannoteEmbeddingProvider.h"

va::thirdparty::speaker::PyannoteEmbeddingOptions options;
options.modelPath = "...";
auto provider = std::make_shared<va::thirdparty::speaker::PyannoteEmbeddingProvider>(options);
// provider->initialize(audioConfig); SpeakerIdentificationProcessor(provider)
```

## How it works

```mermaid
flowchart LR
    In["the utterance's audio (ring buffer)"] --> Processor["SpeakerIdentificationProcessor"]
    Processor -->|"embedding(audio)"| Provider["PyannoteEmbeddingProvider"]
    Provider -->|"Kaldi fbank + ONNX Runtime"| Engine["pyannote_embedding.onnx"]
    Engine --> Provider
    Provider -->|"256-number voice print"| Processor
    Processor --> Out["speaker_identified / speaker_enrollment events"]
```

## Tests

| Test | Kind | Needs | Run |
| --- | --- | --- | --- |
| `pyannote_fbank_parity` | unit | nothing (goldens in the repo) | `ctest --test-dir build -R pyannote_fbank_parity` |
| `vasdk_dsp_speaker` | integration: by config id, and a session with it | `pyannote_embedding.onnx` (skipped without) | `ctest --test-dir build -R vasdk_dsp_speaker` |
| `vasdk_eval_speaker_pyannote` | evaluation: every pair of LibriSpeech clips, same speaker or not ([Evaluation](../evaluation.md)): EER 0% on 10 speakers | the evaluation set (skipped without) | `ctest --test-dir build -L eval` |

A test that needs a model or a key reports "skipped" without it.
