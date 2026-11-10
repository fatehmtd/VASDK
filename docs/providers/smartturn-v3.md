# SmartTurn v3

*Turn detection provider, config id `smartturn-v3`, target `vasdk_smartturn_v3`, build flag `-DVASDK_BUILD_SMART_TURN_V3=ON`. Source: [`VASDK_3P_IMPL/SmartTurnV3/`](../../VASDK_3P_IMPL/SmartTurnV3/). Interface: [`TurnDetectionProvider.h`](../../VASDK/include/VASDK/TurnDetectionProvider.h).*

Pipecat's SmartTurn v3: a Whisper-feature ONNX model that scores whether the user has finished their turn, so a clearly finished turn ends at the pause and an unfinished one waits. On by default.

## Platforms

| macOS | Linux | Windows | Android |
| --- | --- | --- | --- |
| yes | yes | yes | yes |

## Options

In C++, `va::thirdparty::turn::SmartTurnV3Options` (in [`SmartTurnV3Provider.h`](../../VASDK_3P_IMPL/SmartTurnV3/SmartTurnV3Provider.h)). Every field has a default except the ones marked "required" in its comment.

| Field | Default | Meaning |
| --- | --- | --- |
| `modelPath` | `""` | smart_turn_v3.onnx (downloaded by VASDK_3P_IMPL/cmake/DownloadSmartTurnModel.cmake). |
| `threshold` | `0.5f` | End-of-turn probability (0-1) that ends the turn. |
| `maxAudioDurationSeconds` | `8.0f` | Longest stretch of the turn the model looks at, from its end backwards. |
| `postAudioFrames` | `3` | Audio frames after the VAD end the model waits for before its last evaluation. |
| `evaluateOnPause` | `true` | Also evaluate while the user pauses, so a clearly finished turn ends at the pause (TurnDetectionProvider::setEvaluateOnPause). |
| `pauseThreshold` | `0.85f` | End-of-turn probability (0-1) a pause needs to end the turn (TurnDetectionProvider::setPauseThreshold). |

In an engine config, its own `SmartTurnOptions` message ([options.proto](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto)) fills them: a field the config leaves unset falls back to its environment variable, then to the struct's default.

| Config field | Environment variable | Sets |
| --- | --- | --- |
| `model_path` | - | `modelPath` |
| `threshold` | - | `threshold` |
| `max_audio_duration_seconds` | - | `maxAudioDurationSeconds` |
| `post_audio_frames` | - | `postAudioFrames` |
| `evaluate_on_pause` | - | `evaluateOnPause` |
| `pause_threshold` | - | `pauseThreshold` |

## Models and licenses

| Files | Source | License | Fetched by |
| --- | --- | --- | --- |
| smart_turn_v3.onnx | [pipecat-ai/smart-turn-v3](https://huggingface.co/pipecat-ai/smart-turn-v3) | BSD 2-Clause | `VASDK_3P_IMPL/cmake/DownloadSmartTurnModel.cmake` |

FFT: [marton78/pffft](https://github.com/marton78/pffft), NCAR/UCAR license.

All of them are listed in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) and in [Providers, sources and licenses](../provider-catalog.md#sources-and-licenses). An app that ships this provider ships these licenses with it ([vasdk-app](../../skills/vasdk-app/SKILL.md#licenses-ship-with-the-app)).

## Usage

From an engine config:

```textproto
providers {
  turn {
    id: "smartturn-v3"
    options { [type.googleapis.com/vasdk.providers.SmartTurnOptions] { model_path: "models/smart_turn_v3.onnx" } }
  }
}
```

From C++:

```cpp
#include "SmartTurnV3/SmartTurnV3Provider.h"

va::thirdparty::turn::SmartTurnV3Options options;
options.modelPath = "...";
auto provider = std::make_shared<va::thirdparty::turn::SmartTurnV3Provider>(options);
// TurnDetectionProcessor(provider), after provider->initialize(audioConfig, ringBuffer)
```

## How it works

```mermaid
flowchart LR
    In["VADStateFrame, AudioFrame"] --> Processor["TurnDetectionProcessor"]
    Processor -->|"evaluate(frame, agentIsSpeaking)"| Provider["SmartTurnV3Provider"]
    Provider -->|"Whisper features + ONNX Runtime"| Engine["smart_turn_v3.onnx"]
    Engine --> Provider
    Provider -->|"TurnEvaluation"| Processor
    Processor --> Out["TurnStateFrame (TURN_START, TURN_END)"]
```

## Tests

| Test | Kind | Needs | Run |
| --- | --- | --- | --- |
| `smartturn_features_parity` | unit | nothing (goldens in the repo) | `ctest --test-dir build -R smartturn_features_parity` |
| `vasdk_eval_turn_smartturn_v3` | evaluation: finished sentences and sentences stopped mid-phrase, the VAD's states at their real timings ([Evaluation](../evaluation.md)). Finished ones end the turn 80 ms after the speech; 75% of stopped ones end it before the VAD would, 30% within 0.5 s | the evaluation set (skipped without) | `ctest --test-dir build -L eval` |

A test that needs a model or a key reports "skipped" without it.
