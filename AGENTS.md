# Working on VASDK

VASDK is a C++17 framework for voice assistants: a pipeline of typed frames through processors, with swappable providers (VAD, STT, turn detection, LLM, TTS, …) that run locally on ONNX/GGUF models or in the cloud. This file is the entry point for coding agents (Codex, Claude Code, Cursor, Copilot, Gemini, …) and for contributors. The skills in [`skills/`](skills/README.md) cover each task in depth.

## Build, test, run

```bash
cmake -B build -S . -DVASDK_BUILD_TESTS=ON          # add -DVASDK_BUILD_<NAME>=ON per optional provider
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure          # tests needing models or API keys report "skipped" (exit 77)
./build/VASDK_APP/VASDK_Pipeline --config VASDK_APP/config/engine.txtpb --env-file .env
```

Prerequisites, CMake options and model downloads: [docs/building.md](docs/building.md). With Visual Studio or Xcode, binaries land under an extra `Release/` folder.

## Layout

| Path | What |
| --- | --- |
| `VASDK/` | Core library: frames, processors, pipeline, provider interfaces (`VASDK/include/VASDK/`), engine, agents, LLM, C API, tests (`VASDK/tests/`) |
| `VASDK_3P_IMPL/` | Third-party integrations, one CMake target each, guarded by `VASDK_BUILD_<NAME>`; `ProviderCatalog/` registers them by config id |
| `VASDK_APP/`, `VASDK_CALLCENTER_APP/`, `VASDK_CAR_APP/`, `VASDK_PYTHON_APP/`, `VASDK_ANDROID/` | Example apps ([docs/apps](docs/apps/README.md)) |
| `bindings/python/` | Python binding over the C API |
| `docs/` | Reference and guides ([index](docs/README.md)) |
| `skills/` | Task skills for agents and contributors ([index](skills/README.md)) |

## Skills: read the one for your task first

| Task | Skill |
| --- | --- |
| Any new provider (shared touch list, code rules, doc page) | [vasdk-provider](skills/vasdk-provider/SKILL.md) |
| Voice activity detection | [vasdk-vad](skills/vasdk-vad/SKILL.md) |
| Speech-to-text | [vasdk-stt](skills/vasdk-stt/SKILL.md) |
| Text-to-speech | [vasdk-tts](skills/vasdk-tts/SKILL.md) |
| End-of-turn detection | [vasdk-turn-detection](skills/vasdk-turn-detection/SKILL.md) |
| Wake word | [vasdk-wake-word](skills/vasdk-wake-word/SKILL.md) |
| LLM, tools, prompts | [vasdk-llm](skills/vasdk-llm/SKILL.md) |
| Intents and entities (sentence encoder, entity model) | [vasdk-nlu](skills/vasdk-nlu/SKILL.md) |
| Mic, speaker, audio DSP | [vasdk-audio-io](skills/vasdk-audio-io/SKILL.md) |
| Speaker identification | [vasdk-speaker-id](skills/vasdk-speaker-id/SKILL.md) |
| Transports (local, WebSocket, HTTP, WebRTC, mock) | [vasdk-transport](skills/vasdk-transport/SKILL.md) |
| A new core component (stage, processor, frame) | [vasdk-core-component](skills/vasdk-core-component/SKILL.md) |
| A scripted conversation (agent file, goals, entities) | [vasdk-agent](skills/vasdk-agent/SKILL.md) |
| A use case: agent, car vertical or new app | [vasdk-app](skills/vasdk-app/SKILL.md) |
| Exposing a feature in C, Python and Android | [vasdk-bindings](skills/vasdk-bindings/SKILL.md) |

## House rules

1. **Headers are the source of truth.** The docs say which header they document; when prose and code disagree, the header wins, and the doc gets fixed.
2. **SDK features belong in the core.** A new capability is a core component plus 3P implementations, usable by every app, never a shortcut inside one app.
3. **Interfaces grow by optional virtuals with safe defaults**, so existing implementations keep compiling.
4. **New core components follow the existing ones**: a generic provider interface in `VASDK/include/VASDK/` plus a processor that drives any implementation of it (as `SpeakerEmbeddingProvider.h` + `SpeakerIdentificationProcessor.h`). The interface is generic enough that a 3P implementation is self-contained: it implements the interface and nothing else.
5. **Every platform.** A feature reaches the C API, Python and Android ([vasdk-bindings](skills/vasdk-bindings/SKILL.md)).
6. **Full names**, no abbreviations: `_backchannelConfig`, not `_bc`.

### Third-party integrations

1. **Namespace `va::thirdparty::<category>`**, the category of its stage: `vad`, `stt`, `tts`, `turn`, `wakeword`, `llm`, `nlu`, `speaker`, `audio`, `transport` ([table](skills/vasdk-provider/SKILL.md#code-rules)); an in-tree port's internals one level down (`va::thirdparty::tts::phonon`). Since `audio`, `transport`, `llm` and `nlu` are also core namespaces, 3P code writes the core's names in full (`va::audio::AudioConfig`). Only the catalog sits in `va::thirdparty` itself.
2. **No `static` variables in source files**, at file scope or inside functions. State lives in the instance, so several instances and sessions run side by side; fixed tables are `constexpr`, helpers go in an anonymous namespace. `ctest` checks rules 1 and 2 (`vasdk_thirdparty_rules`, `VASDK_3P_IMPL/check_rules.py`); its one exemption wraps a library that is itself process-global (Whistle's needle engine).
3. **Config and parameters are an options struct with a default for every field** (`<Name>Options`, next to the class), mirrored by an options message in `VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto` (shared ones such as `CloudOptions` where they fit). Resolution: config field, then the provider's environment variable, then the struct's default; the catalog never repeats a default.
4. **Cross-platform: macOS, Linux, Windows, Android.** Where a backend can't run on one, CMake turns it off there with `message(WARNING "<Name>: not supported on <platform>: <why>")`, selecting it at run time logs a warning, and the gap is written in its doc page, `docs/provider-catalog.md` and the README provider table.
5. **Its own doc page**: `docs/providers/<id>.md`, from [the template](skills/vasdk-provider/provider-doc-template.md), with options, a usage example and a mermaid flow.

## Definition of done

A change is finished when all of these hold, not before:

1. **Documented.** The reference page that owns the logic is updated, the header comments state the contract, and a new component or integration has its own page with its options, a usage example and a mermaid flow.
2. **README updated** where the change shows: the provider table, the app table, "on by default", the quick start.
3. **Licenses recorded.** Every new library, model or dataset is in `THIRD_PARTY_NOTICES.md` and in "Sources and licenses" of `docs/provider-catalog.md`. An app ships the licenses of what it uses ([vasdk-app](skills/vasdk-app/SKILL.md)).
4. **Unit test.** A plain executable registered with `add_test` under `if(VASDK_BUILD_TESTS)` (no test framework): core logic in `VASDK/tests/<feature>_test.cpp`, a provider's own in `VASDK_3P_IMPL/<Name>/tests/`.
5. **Integration test** that drives the real path: a provider through the catalog and a session (as `VASDK_3P_IMPL/ProviderCatalog/offline_conversation_test.cpp`) or a `MockTransport` pipeline; an agent through `<app>_agentc test`; a binding through `bindings/python/tests/`.
6. **Skips, not failures, without models or keys:** such tests exit 77 and set `SKIP_RETURN_CODE 77`.
7. **`ctest` passes**, reported as passed, failed or skipped, never summarized as "should work".
8. **Platforms checked.** Built on the platforms at hand; the others are named as not built. Unsupported platforms have their warning and their note.
9. **The rules above hold.**
