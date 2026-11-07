---
name: vasdk-provider
description: Add a new VASDK provider (VAD, STT, TTS, turn detection, wake word, sentence encoder, entity model, audio DSP, speaker embedding) under VASDK_3P_IMPL. The full list of files a provider touches (CMake option, model download, ProviderCatalog registration, options.proto, .env, Android, docs, licenses), the code rules for third-party code, its doc page and its tests. Use together with the stage skill (vasdk-tts, vasdk-stt, vasdk-vad, ...).
---

# Adding a provider

A provider is the swappable engine behind one pipeline stage. It implements one interface from `VASDK/include/VASDK/` and nothing else: no processor, frame or app code. Read the stage skill first (it names the interface, the contract pitfalls and the template to copy), then follow this list. Interface contracts: [docs/reference/providers.md](../../docs/reference/providers.md).

## Code rules

1. Everything in **`namespace va::thirdparty`**.
2. **No `static` variables in `.cpp` files**, at file scope or in functions. State is a member; fixed tables are `constexpr`.
3. **One `<Name>Options` struct, a default for every field**, passed to the constructor. No long parameter lists, no setters for configuration.
4. **macOS, Linux, Windows, Android.** If one can't work, see "When a platform can't" below.
5. Full names, no abbreviations. Logs through `spdlog` with a `[<Name>]` prefix; never log what the user said at `info` (see `VASDK/include/VASDK/Masking.h`).
6. Async results go out through the stage's listener from your own thread; `stop*()` joins cleanly; no blocking inside callbacks.

Every integration in `VASDK_3P_IMPL/` follows rules 1 to 3, so any of them is a template; the stage skill names the closest one. `VASDK_3P_IMPL/check_rules.py` checks rules 1 and 2 in `ctest` (`vasdk_thirdparty_rules`). Its one exemption is `VASDK_3P_IMPL/WhistleSTT/impl/NeedleEngine.cpp`: the needle engine it wraps is itself process-global, so the wrapper is too. Rule 3 in practice: the struct sits next to the class in its header; the catalog fills only the fields the config or the environment sets (`VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.cpp`), so every default lives in the struct. Settings that change while running (a TTS voice) stay methods; the rest leave no setter.

## Files to touch

| # | File | What |
| --- | --- | --- |
| 1 | `VASDK_3P_IMPL/<Name>/` | `CMakeLists.txt` (target `vasdk_<name>`, links `VASDK`), `<Name>Provider.h/.cpp`, `tests/` |
| 2 | `VASDK_3P_IMPL/CMakeLists.txt` | `option(VASDK_BUILD_<NAME> "..." OFF)`, its dependencies (`FetchContent`, pinned to a tag or commit), `add_subdirectory(<Name>)` |
| 3 | `VASDK_3P_IMPL/cmake/Download<Name>Model.cmake` | Local models only: a `VASDK_DOWNLOAD_<NAME>_MODELS` switch, a pinned revision, files under `models/<name>/`; copy `VASDK_3P_IMPL/cmake/DownloadSupertonicModels.cmake`. Include it from `VASDK_3P_IMPL/CMakeLists.txt` with `NOT ANDROID` |
| 4 | `VASDK_3P_IMPL/ProviderCatalog/CMakeLists.txt` | Add `vasdk_<name>  <NAME>` to `_vasdk_catalog_providers` (defines `VASDK_HAS_<NAME>`) |
| 5 | `VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.cpp` | `#ifdef VASDK_HAS_<NAME>` include, and `registry.add<Interface, providers::<Name>Options>("<id>", ...)` building the provider from the options, using `orEnvironment`, `modelDir` and `configured`. The order of registration is the default preference |
| 6 | `VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.h` | The id in the comment's list |
| 7 | `VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto` | A `<Name>Options` message mirroring the struct, every field optional, each commented with its default and environment variable. Reuse `CloudOptions`, `LocalSttOptions` or `LocalTtsOptions` when they fit exactly |
| 8 | `.env.example` | The API key and every environment override, commented, with the build flag that needs them |
| 9 | `VASDK_APP/CMakeLists.txt`, `VASDK_CALLCENTER_APP/CMakeLists.txt` | Add `VASDK_BUILD_<NAME>` to the "at least one STT/TTS provider" checks |
| 10 | `VASDK_APP/config/engine.txtpb` | When it should be an example: a commented `ProviderSpec` |
| 11 | Android (unless it can't run there) | `VASDK_ANDROID/CMakeLists.txt` (`set(VASDK_BUILD_<NAME> ON CACHE BOOL "" FORCE)`), `VASDK_ANDROID/app/build.gradle`, provider choice in `VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/VasdkEngine.kt` and `VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/ui/SetupWizard.kt`, then `docs/apps/android.md` |
| 12 | `docs/providers/<id>.md` | Its own page, from [provider-doc-template.md](provider-doc-template.md): options, usage example, mermaid flow |
| 13 | `docs/provider-catalog.md` | Its row (linking the page), per-provider notes, "Sources and licenses" |
| 14 | `docs/reference/engine-config.md` | The id in the "Providers" order list, the options message in its table |
| 15 | `README.md` | The provider table, and "On by default" if it is |
| 16 | `THIRD_PARTY_NOTICES.md` | The library (code table) and every model (model table), with source and license; a restrictive license (non-commercial, OpenRAIL) gets its note |

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
- the code rules hold: `va::thirdparty`, no statics in source files, options struct with defaults
- every platform builds, or its gap carries the CMake warning, the runtime warning and the doc notes
