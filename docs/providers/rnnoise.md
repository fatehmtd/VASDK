# RNNoise

*Audio DSP provider, config id `rnnoise` (`providers.audio_dsp`), target `vasdk_rnnoise`, build flag `-DVASDK_BUILD_RNNOISE=ON`. Source: [`VASDK_3P_IMPL/RNNoise/`](../../VASDK_3P_IMPL/RNNoise/). Interface: [`GenericAudioDSPProvider.h`](../../VASDK/include/VASDK/GenericAudioDSPProvider.h).*

Xiph's RNNoise noise suppression: a small recurrent network at 48 kHz, with the source audio resampled in and out. Built-in weights; no download needed at run time. On by default on desktop.

## Platforms

| macOS | Linux | Windows | Android |
| --- | --- | --- | --- |
| yes | yes | yes | no |

## Options

In an engine config, `vasdk.providers.RNNoiseOptions` (`model_path`, `vad_threshold`, `normalize_output`, `normalization_multiplier`), each unset field keeping the default below. In C++, `va::thirdparty::audio::RNNoiseDSPOptions` (in [`RNNoiseDSPProvider.h`](../../VASDK_3P_IMPL/RNNoise/RNNoiseDSPProvider.h)). Every field has a default except the ones marked "required" in its comment.

| Field | Default | Meaning |
| --- | --- | --- |
| `modelPath` | `""` | A custom RNNoise model file (.rnnn); empty uses the model built into the library. |
| `vadThreshold` | `0.0f` | RNNoise voice probability (0-1) under which a frame counts as silence, exposed through getLastVADProbability(); 0 disables it. |
| `normalizeOutput` | `false` | Scale the output so its peak stays under normalizationMultiplier of full scale. |
| `normalizationMultiplier` | `0.8f` | Headroom for normalizeOutput: 0.8 is 80% of full scale. |

On Android, CMake prints "RNNoise: not supported on Android" and turns `VASDK_BUILD_RNNOISE` off: its fetch builds for desktop only.

## Models and licenses

| Files | Source | License | Fetched by |
| --- | --- | --- | --- |
| built into the library | [xiph/rnnoise](https://github.com/xiph/rnnoise) | BSD 3-Clause | `VASDK_3P_IMPL/cmake/FetchRNNoise.cmake` |

All of them are listed in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) and in [Providers, sources and licenses](../provider-catalog.md#sources-and-licenses). An app that ships this provider ships these licenses with it ([vasdk-app](../../skills/vasdk-app/SKILL.md#licenses-ship-with-the-app)).

## Usage

In an engine config: every stage of the session (wake word, VAD, turn model, STT, speaker identification) then hears the denoised audio.

```textproto
providers { audio_dsp { id: "rnnoise" } }
```

In a pipeline built by hand:

```cpp
#include "RNNoise/RNNoiseDSPProvider.h"

va::thirdparty::audio::RNNoiseDSPOptions options;
options.modelPath = "...";
auto provider = std::make_shared<va::thirdparty::audio::RNNoiseDSPProvider>(options);
// provider->initialize(audioConfig); AudioDSPProcessor(provider)
```

## How it works

```mermaid
flowchart LR
    In["raw audio (the transport's ring buffer)"] --> Processor["AudioDSPProcessor"]
    Processor -->|"processAudio(input, output, samples)"| Provider["RNNoiseDSPProvider"]
    Provider -->|"rnnoise_process_frame (48 kHz, 10 ms frames)"| Engine["RNNoise (built-in model)"]
    Engine --> Provider
    Provider -->|"denoised audio"| Processor
    Processor --> Out["processed ring buffer"]
    Out --> Stages["wake word, VAD, turn, STT, speaker id"]
```

## Tests

| Test | Kind | Needs | Run |
| --- | --- | --- | --- |
| `vasdk_rnnoise` | unit | nothing | `ctest --test-dir build -R vasdk_rnnoise` |
| `vasdk_dsp_speaker` | integration: by config id, and a session with it | pyannote's model (skipped without) | `ctest --test-dir build -R vasdk_dsp_speaker` |

A test that needs a model or a key reports "skipped" without it.
