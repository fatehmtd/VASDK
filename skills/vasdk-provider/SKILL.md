---
name: vasdk-provider
description: Add a new VASDK provider (VAD, STT, TTS, turn detection, wake word, sentence encoder, entity model, audio DSP, speaker embedding) under VASDK_3P_IMPL. The full list of files a provider touches (CMake option, model download, ProviderCatalog registration, options.proto, .env, Android, docs, licenses), the code rules for third-party code, its doc page and its tests. Use together with the stage skill (vasdk-tts, vasdk-stt, vasdk-vad, ...).
---

# Adding a provider

A provider is the swappable engine behind one pipeline stage. It implements one interface from `VASDK/include/VASDK/` and nothing else: no processor, frame or app code. Read the stage skill first (it names the interface, the contract pitfalls and the template to copy), then follow this list. Interface contracts: [docs/reference/providers.md](../../docs/reference/providers.md).

## Code rules

1. Everything in **`namespace va::thirdparty::<category>`**, the category of its stage (table below); an in-tree port's internals go one level further down (`va::thirdparty::tts::phonon`). Inside it, write the core's names in full (`va::audio::AudioConfig`, `va::llm::LLMConfig`): `audio`, `transport`, `llm` and `nlu` are categories too, so a bare `audio::` would find `va::thirdparty::audio`.
2. **No `static` variables in `.cpp` files**, at file scope or in functions. State is a member; fixed tables are `constexpr`.
3. **One `<Name>Options` struct, a default for every field**, passed to the constructor. No long parameter lists, no setters for configuration.
4. **macOS, Linux, Windows, Android.** If one can't work, see "When a platform can't" below.
5. Full names, no abbreviations. Logs through `spdlog` with a `[<Name>]` prefix; never log what the user said at `info` (see `VASDK/include/VASDK/Masking.h`).
6. Async results go out through the stage's listener from your own thread; `stop*()` joins cleanly; no blocking inside callbacks.

| Category | Stage skill | Integrations in it |
| --- | --- | --- |
| `vad` | [vasdk-vad](../vasdk-vad/SKILL.md) | SileroVAD, EnergyZCRVAD |
| `stt` | [vasdk-stt](../vasdk-stt/SKILL.md) | Cartesia, Deepgram, Fish Audio, Gladia, Gradium, Soniox, Whistle, Moonshine, Parakeet, whisper.cpp, Voz |
| `tts` | [vasdk-tts](../vasdk-tts/SKILL.md) | Cartesia, Deepgram, Fish Audio, Gradium, Soniox, Supertonic, Pocket, Phonon, MOSS |
| `turn` | [vasdk-turn-detection](../vasdk-turn-detection/SKILL.md) | SmartTurnV3 |
| `wakeword` | [vasdk-wake-word](../vasdk-wake-word/SKILL.md) | none yet |
| `llm` | [vasdk-llm](../vasdk-llm/SKILL.md) | LlamaCpp |
| `nlu` | [vasdk-nlu](../vasdk-nlu/SKILL.md) | MiniLmNlu, GlinerNer |
| `speaker` | [vasdk-speaker-id](../vasdk-speaker-id/SKILL.md) | PyannoteSpeaker |
| `audio` | [vasdk-audio-io](../vasdk-audio-io/SKILL.md) | MiniAudio, SDL, FFmpegAudioSource, RNNoise, VizAnim |
| `transport` | [vasdk-transport](../vasdk-transport/SKILL.md) | LibDataChannelWebRtc, FileTransport (in FFmpegAudio) |

The catalog (`VASDK_3P_IMPL/ProviderCatalog/`) registers every category, so it sits in `va::thirdparty` itself and names integrations by category (`stt::WhistleTranscriptionProvider`). Apps do the same: `std::make_shared<va::thirdparty::audio::MiniAudioSource>()`. A folder whose files serve two stages puts each file in its own category (FFmpegAudio). A new kind of integration adds its category to `CATEGORIES` in `VASDK_3P_IMPL/check_rules.py`, to this table and to `AGENTS.md`.

Every integration in `VASDK_3P_IMPL/` follows rules 1 to 3, so any of them is a template; the stage skill names the closest one. `VASDK_3P_IMPL/check_rules.py` checks rules 1 and 2 in `ctest` (`vasdk_thirdparty_rules`): the category namespace, the core's names written in full, no statics. Its one exemption is `VASDK_3P_IMPL/WhistleSTT/impl/NeedleEngine.cpp`: the needle engine it wraps is itself process-global, so the wrapper is too. Rule 3 in practice: the struct sits next to the class in its header; the catalog fills only the fields the config or the environment sets (`VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.cpp`), so every default lives in the struct. Settings that change while running (a TTS voice) stay methods; the rest leave no setter.

## Files to touch

| # | File | What |
| --- | --- | --- |
| 1 | `VASDK_3P_IMPL/<Name>/` | `CMakeLists.txt` (target `vasdk_<name>`, links `VASDK`), `<Name>Provider.h/.cpp`, `tests/` |
| 2 | `VASDK_3P_IMPL/CMakeLists.txt` | `option(VASDK_BUILD_<NAME> "..." OFF)`, its dependencies (`FetchContent`, pinned to a tag or commit), `add_subdirectory(<Name>)` |
| 3 | `VASDK_3P_IMPL/cmake/Download<Name>Model.cmake` | Local models only: a `VASDK_DOWNLOAD_<NAME>_MODELS` switch and a `<NAME>_MODEL_DIR`, then `vasdk_download_model(<manifest id> DIRECTORY ${<NAME>_MODEL_DIR} STRIP <name>/)` ([`VASDKModels.cmake`](../../VASDK_3P_IMPL/cmake/VASDKModels.cmake)), which downloads the files row 18 pins; copy `VASDK_3P_IMPL/cmake/DownloadSupertonicModels.cmake`. No URL or hash in the script: the manifest holds them, and `vasdk_model_downloads` fails on an id it lacks. Include it from `VASDK_3P_IMPL/CMakeLists.txt` with `NOT ANDROID`, and add the switch to the list `VASDK_DOWNLOAD_MODELS` turns off there. A file the build compiles against (a header) is no model: fetch it regardless |
| 4 | `VASDK_3P_IMPL/ProviderCatalog/CMakeLists.txt` | Add `vasdk_<name>  <NAME>` to `_vasdk_catalog_providers` (defines `VASDK_HAS_<NAME>`) |
| 5 | `VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.cpp` | `#ifdef VASDK_HAS_<NAME>` include, and `registry.add<Interface, providers::<Name>Options>("<id>", ...)` building the provider from the options, using `orEnvironment`, `modelDir` and `configured`. The order of registration is the default preference |
| 6 | `VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.h` | The id in the comment's list |
| 7 | `VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto` | Its own `<Name>Options` message mirroring every field of the struct, every scalar `optional` (unset: the environment variable, then the struct's default), each commented with its default and environment variable; the catalog registers it with `registry.add<Interface, <Name>Options>`. Don't reuse the shared `CloudOptions`, `LocalSttOptions` or `LocalTtsOptions`: they stay only for older configs (`registry.add<Interface, Own, Shared>` converts them) |
| 8 | `.env.example` | The API key and every environment override, commented, with the build flag that needs them |
| 9 | `VASDK_APP/CMakeLists.txt`, `VASDK_CALLCENTER_APP/CMakeLists.txt` | Add `VASDK_BUILD_<NAME>` to the "at least one STT/TTS provider" checks |
| 10 | `VASDK_APP/config/engine.txtpb` | When it should be an example: a commented `ProviderSpec` |
| 11 | Android (unless it can't run there) | `VASDK_ANDROID/CMakeLists.txt` (`set(VASDK_BUILD_<NAME> ON CACHE BOOL "" FORCE)`), `VASDK_ANDROID/app/build.gradle`, provider choice in `VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/VasdkEngine.kt` and `VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/ui/SetupWizard.kt`, then `docs/apps/android.md` |
| 12 | `docs/providers/<id>.md` | Its own page, from [provider-doc-template.md](provider-doc-template.md): options, usage example, mermaid flow |
| 13 | `docs/provider-catalog.md` | Its row (linking the page), per-provider notes, "Sources and licenses" |
| 14 | `docs/reference/engine-config.md` | The id in the "Providers" order list, the options message in its table |
| 15 | `README.md` | The provider table, and "On by default" if it is |
| 16 | `THIRD_PARTY_NOTICES.md` | The library (code table) and every model (model table), with source and license; a restrictive license (non-commercial, OpenRAIL) gets its note |
| 17 | `cmake/VASDKFlavors.cmake` | Its flavor ([Flavors](../../docs/integrating.md#flavors)): `default` only when it builds on every desktop platform with no system library and its code and default model are permissively licensed; `full` otherwise; neither when it needs a platform SDK or system library (say why in the comment). The catalog gives it the target `VASDK::<name>` and a `find_package(VASDK COMPONENTS <name>)` component by itself. It reaches the installed C++ package (`COMPONENTS cpp`) only if it links nothing the build downloads or compiles in ([packaging](../vasdk-packaging/SKILL.md), rule 6) |
| 18 | `VASDK_3P_IMPL/models/generate_manifest.py` | Local models only: a `model(...)` entry (id, `<stage>/<id>` in `providers`, license, pinned revision, files at the path the provider reads under `models_dir`), then run it to rewrite `VASDK_3P_IMPL/models/manifest.json` with the sizes and SHA-256s. `requires_acceptance=True` for a non-commercial or custom license, `gated=True` when the host wants a token. The model store and `vasdk models pull` download from it ([Models](../../docs/models.md)) |

Real examples of the whole list: `git show --stat 2a31dc4` (three cloud STT/TTS pairs), and `git grep -il supertonic` (a local TTS with model downloads).

## When a platform can't

- CMake: in `VASDK_3P_IMPL/CMakeLists.txt`, `if(VASDK_BUILD_<NAME> AND <platform>)` → `message(WARNING "<Name>: not supported on <platform>: <why>")` and turn the option off, as FFmpeg does when it isn't found.
- Run time: when the catalog id is asked for on that platform, log a warning naming the platform and return `nullptr`, so the `ProviderSpec.fallback` list takes over.
- Docs: a "Platforms" line on its page, a note in `docs/provider-catalog.md`, and "(not on <platform>)" after its name in the README provider table.

## Tests

- **Unit**, `VASDK_3P_IMPL/<Name>/tests/<name>_test.cpp`: a plain `main()` returning non-zero on failure, registered in its CMakeLists under `if(VASDK_BUILD_TESTS)` with `add_test` and `set_tests_properties(... SKIP_RETURN_CODE 77)`. Exit 77 when its model or `*_API_KEY` is missing. On Windows call `vasdk_test_dlls(<test target>)`. Examples: `VASDK_3P_IMPL/SupertonicTTS/tests/synth_test.cpp`, `VASDK_3P_IMPL/SmartTurnV3/tests/features_test.cpp` (golden outputs), `VASDK_3P_IMPL/MoonshineSTT/tests/stream_test.cpp`.
- **Options**: a case in the unit test that builds it through `ProviderRegistry` with an engine config that sets every option, and one with none set (the defaults).
- **Integration**: the provider in a real session through the catalog: `VASDK_3P_IMPL/ProviderCatalog/offline_conversation_test.cpp` drives VAD, STT, agent and TTS over the C API; add the new provider as a variant there or write a sibling test next to it. Cloud providers skip without their key.

```bash
cmake -B build -S . -DVASDK_BUILD_TESTS=ON -DVASDK_BUILD_<NAME>=ON
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure -R "<name>|offline_conversation|provider_registry"
./build/VASDK_APP/VASDK_Pipeline --config my-engine.txtpb   # with providers { <stage> { id: "<id>" } }
```

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds, and also:
- all 16 rows above are handled or marked not applicable
- the code rules hold: `va::thirdparty::<category>`, no statics in source files, options struct with defaults
- every platform builds, or its gap carries the CMake warning, the runtime warning and the doc notes
