# Gladia (STT, WIP)

*Speech-to-text provider, config id `gladia`, target `vasdk_gladiapp_transcription`, build flag `-DVASDK_BUILD_GLADIAPP=ON`. Source: [`VASDK_3P_IMPL/GladiappTranscription/`](../../VASDK_3P_IMPL/GladiappTranscription/). Interface: [`GenericTranscriptionProvider.h`](../../VASDK/include/VASDK/GenericTranscriptionProvider.h).*

Gladia's real-time speech recognition, through [gladiapp](https://github.com/fatehmtd/gladiapp). Work in progress: expect rough edges. It needs a network connection and a Gladia API key.

## Platforms

| macOS | Linux | Windows | Android |
| --- | --- | --- | --- |
| no (build issue) | no (build issue) | no (build issue) | yes |

## Options

In C++, `va::thirdparty::stt::GladiaPPTranscriptionOptions` (in [`GladiaPPTranscriptionProvider.h`](../../VASDK_3P_IMPL/GladiappTranscription/GladiaPPTranscriptionProvider.h)). Every field has a default except the ones marked "required" in its comment.

| Field | Default | Meaning |
| --- | --- | --- |
| `apiKey` | `""` | API key of the service; required (the catalog falls back to the service's *_API_KEY variable). |
| `caBundlePath` | `""` | PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). |

In an engine config, the catalog's `CloudOptions` message ([options.proto](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto)) fills them: a field the config leaves unset falls back to its environment variable, then to the struct's default.

| Config field | Environment variable | Sets |
| --- | --- | --- |
| `api_key` | `GLADIA_API_KEY` | `apiKey` |
| `ca_bundle_path` | `VASDK_CA_BUNDLE` | `caBundlePath` |

Known build issue: on desktop, `gladiapp` fetches its own copy of dotenv-cpp, which clashes with VASDK's, so configuring with `-DVASDK_BUILD_GLADIAPP=ON` fails there today. The Android build, which doesn't fetch dotenv-cpp, isn't affected.

## Models and licenses

Client: [fatehmtd/gladiapp](https://github.com/fatehmtd/gladiapp), MIT. Service: [gladia.io](https://www.gladia.io), under your Gladia account's terms.

All of them are listed in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) and in [Providers, sources and licenses](../provider-catalog.md#sources-and-licenses). An app that ships this provider ships these licenses with it ([vasdk-app](../../skills/vasdk-app/SKILL.md#licenses-ship-with-the-app)).

## Usage

From an engine config:

```textproto
providers {
  stt {
    id: "gladia"
    options { [type.googleapis.com/vasdk.providers.CloudOptions] { api_key: "${GLADIA_API_KEY}" } }
    fallback: "whistle"
  }
}
```

From C++:

```cpp
#include "GladiappTranscription/GladiaPPTranscriptionProvider.h"

va::thirdparty::stt::GladiaPPTranscriptionOptions options;
options.apiKey = "...";
auto provider = std::make_shared<va::thirdparty::stt::GladiaPPTranscriptionProvider>(options);
// TranscriptionFrameProcessor(provider); setAudioConfiguration() before the pipeline starts
```

## How it works

```mermaid
flowchart LR
    In["AudioFrame while the user speaks"] --> Processor["TranscriptionFrameProcessor"]
    Processor -->|"writeAudioData() / commitEndOfSpeech()"| Provider["GladiaPPTranscriptionProvider"]
    Provider -->|"WebSocket / HTTPS"| Engine["Gladia API"]
    Engine --> Provider
    Provider -->|"onTranscriptionResult (partial, final)"| Processor
    Processor --> Out["TranscriptionFrame"]
```

## Tests

None of its own yet: it needs an API key and a network. The [provider skill](../../skills/vasdk-provider/SKILL.md#tests) describes the unit and integration tests a provider should have.
