# Engine config files

Source of truth: [`engine_config.proto`](../../VASDK/proto/vasdk/engine/engine_config.proto), [`ProtoFileLoader.h`](../../VASDK/include/VASDK/engine/ProtoFileLoader.h), [`ProviderRegistry.h`](../../VASDK/include/VASDK/engine/ProviderRegistry.h), [`options.proto`](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto).

An engine config picks the VAD, STT and TTS providers and the LLM settings at run time, so switching from Whistle to Parakeet, or from PocketTTS to Cartesia, is a file edit instead of a rebuild. The schema is a protobuf message (`vasdk.engine.EngineConfig`); you write it as text proto.

```textproto
# proto-file: vasdk/engine/engine_config.proto
# proto-message: vasdk.engine.EngineConfig
providers {
  vad { id: "silero" }
  stt { id: "whistle" fallback: ["parakeet"] }
  tts {
    id: "cartesia"
    options { [type.googleapis.com/vasdk.providers.CloudOptions] { api_key: "${CARTESIA_API_KEY}" } }
    fallback: ["pocket"]
  }
}
llm { endpoint: "${LLM_ENDPOINT:-https://openrouter.ai/api/v1/chat/completions}" model: "${LLM_MODEL}" }
```

Run it with `VASDK_Pipeline --config my-engine.txtpb`. A commented example ships in [`VASDK_APP/config/engine.txtpb`](../../VASDK_APP/config/engine.txtpb).

## Formats

| Extension | Format | Typical writer |
| --- | --- | --- |
| `.txtpb`, `.textproto`, `.pbtxt` | Text proto | People |
| `.json` | [Proto3 JSON](https://protobuf.dev/programming-guides/json/) (`modelsDir`, `"@type"` for options) | Tools, web UIs |
| `.pb`, `.binpb` | Binary | Build steps, mobile apps |

Unknown fields, misspelled names and wrong types are errors. Text-proto errors give the line and column: a misspelled `providrs { }` on line 4 fails with `engine.txtpb:4:10: Message type "vasdk.engine.EngineConfig" has no field named "providrs".`

## Environment variables

Inside quoted strings, `${NAME}` is replaced with the environment variable `NAME`, and `${NAME:-fallback}` uses `fallback` when `NAME` is unset. An unset `NAME` with no fallback fails the load and names the variable and line. Comments are never expanded. Values are escaped, so a quote in a secret cannot end the string it lands in. VASDK_Pipeline loads `.env` first, so variables from it work here. Keep secrets in the environment, not in the file.

## Providers

Each `ProviderSpec` has:

- `id`: which provider to create. When empty, the first one built into the binary is used, in this order. VAD: `silero`. STT: `gradium`, `soniox`, `deepgram`, `cartesia`, `fishaudio`, `whistle`, `moonshine`, `parakeet`. TTS: `gradium`, `soniox`, `supertonic`, `deepgram`, `cartesia`, `fishaudio`, `moss`, `phonon`, `pocket`. This order matches what VASDK_Pipeline picked before config files existed, so running without `--config` behaves as before.
- `options`: the provider's own options message, written with the `Any` syntax `[type.googleapis.com/<message>] { ... }`. Every field is optional. Unset fields fall back to the provider's environment variable (`SONIOX_API_KEY`, `PHONON_TTS_VOICE`, …), then to its default.

  | Message | Providers | Fields |
  | --- | --- | --- |
  | `vasdk.providers.CloudOptions` | cloud STT/TTS | `api_key`, `voice_id` |
  | `vasdk.providers.SileroOptions` | `silero` | `model_path` |
  | `vasdk.providers.LocalSttOptions` | `whistle`, `moonshine`, `parakeet` | `model_dir`, `idle_timeout_ms`, `partial_interval_ms`, `language` |
  | `vasdk.providers.LocalTtsOptions` | `supertonic`, `moss`, `phonon`, `pocket` | `model_dir`, `voice`, `language`, `steps` |

- `fallback`: ids to try, in order, when `id` is not built into the binary or fails to start (a missing model file, for example). Options apply to `id` only; a fallback starts with its defaults. The log shows which provider was used and why the others were skipped.

`models_dir` sets the base directory for local model files. When it is unset, VASDK_Pipeline uses the `models/` folder it finds next to the executable. A relative path is resolved from the current directory.

## Using it from C++

```cpp
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/engine/ProviderRegistry.h>
#include "ProviderCatalog/ProviderCatalog.h"   // link vasdk_provider_catalog

vasdk::engine::EngineConfig config;
std::string error;
if (!va::engine::loadEngineConfig("engine.txtpb", config, &error)) { spdlog::error(error); return 1; }

va::engine::ProviderRegistry registry;
va::thirdparty::registerAllProviders(registry);         // every provider built into this binary

va::engine::ProviderContext input{"models", inputAudioConfig, frameSize};
auto stt = registry.create<va::audio::GenericTranscriptionProvider>(config.providers().stt(), input, &error);
```

To make your own provider selectable by id, register a factory. Give it a typed options message if it has settings:

```cpp
registry.add<va::audio::GenericVoiceSynthesisProvider, acme::MyTtsOptions>("acme",
    [](const acme::MyTtsOptions& options, const va::engine::ProviderContext& context) {
        auto tts = std::make_shared<AcmeTts>(options.voice());
        tts->setAudioConfiguration(context.audio);
        return tts;
    });
```

The options message has to be compiled into the binary, so the loader can resolve its `[type.googleapis.com/acme.MyTtsOptions]` name. Use `loadProtoFile()` for other messages; it expands `${...}` only when asked.

## Build

Engine config files need protobuf. Configure uses an installed protobuf (Homebrew `protobuf`, vcpkg, or a system package with CMake config files) and otherwise builds v36.2 from source. The option `VASDK_ENABLE_ENGINE_CONFIG` defaults to ON on desktop and OFF on Android, where the size cost is still being evaluated. VASDK_Pipeline requires it.
