# Cartesia (STT)

*Speech-to-text provider, config id `cartesia`, target `vasdk_cartesiapp_stt`, build flag `-DVASDK_BUILD_CARTESIAPP_STT=ON`. Source: [`VASDK_3P_IMPL/CartesiappTranscription/`](../../VASDK_3P_IMPL/CartesiappTranscription/). Interface: [`GenericTranscriptionProvider.h`](../../VASDK/include/VASDK/GenericTranscriptionProvider.h).*

Cartesia's streaming speech recognition (ink-whisper) over WebSocket, through [CartesiaPP](https://github.com/fatehmtd/CartesiaPP). It needs a network connection and a Cartesia API key.

## Platforms

| macOS | Linux | Windows | Android |
| --- | --- | --- | --- |
| yes | yes | yes | yes |

## Options

In C++, `va::thirdparty::CartesiappTranscriptionOptions` (in [`CartesiappTranscriptionProvider.h`](../../VASDK_3P_IMPL/CartesiappTranscription/CartesiappTranscriptionProvider.h)). Every field has a default except the ones marked "required" in its comment.

| Field | Default | Meaning |
| --- | --- | --- |
| `apiKey` | `""` | API key of the service; required (the catalog falls back to the service's *_API_KEY variable). |
| `model` | `cartesiapp::stt::models::INK_WHISPER` | Model to use. |
| `language` | `""` | Language code; empty lets the service detect it or use its default. |
| `caBundlePath` | `""` | PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). |

In an engine config, the catalog's `CloudOptions` message ([options.proto](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto)) fills them: a field the config leaves unset falls back to its environment variable, then to the struct's default.

| Config field | Environment variable | Sets |
| --- | --- | --- |
| `api_key` | `CARTESIA_API_KEY` | `apiKey` |
| `ca_bundle_path` | `VASDK_CA_BUNDLE` | `caBundlePath` |

The engine config sets the key and the certificate bundle; `model` and `language` are set in C++.

## Models and licenses

Client: [fatehmtd/CartesiaPP](https://github.com/fatehmtd/CartesiaPP), Apache 2.0. Service: [cartesia.ai](https://cartesia.ai), under your Cartesia account's terms.

All of them are listed in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) and in [Providers, sources and licenses](../provider-catalog.md#sources-and-licenses). An app that ships this provider ships these licenses with it ([vasdk-app](../../skills/vasdk-app/SKILL.md#licenses-ship-with-the-app)).

## Usage

From an engine config:

```textproto
providers {
  stt {
    id: "cartesia"
    options { [type.googleapis.com/vasdk.providers.CloudOptions] { api_key: "${CARTESIA_API_KEY}" } }
    fallback: "whistle"
  }
}
```

From C++:

```cpp
#include "CartesiappTranscription/CartesiappTranscriptionProvider.h"

va::thirdparty::CartesiappTranscriptionOptions options;
options.apiKey = "...";
auto provider = std::make_shared<va::thirdparty::CartesiappTranscriptionProvider>(options);
// TranscriptionFrameProcessor(provider); setAudioConfiguration() before the pipeline starts
```

## How it works

```mermaid
flowchart LR
    In["AudioFrame while the user speaks"] --> Processor["TranscriptionFrameProcessor"]
    Processor -->|"writeAudioData() / commitEndOfSpeech()"| Provider["CartesiappTranscriptionProvider"]
    Provider -->|"WebSocket / HTTPS"| Engine["Cartesia API"]
    Engine --> Provider
    Provider -->|"onTranscriptionResult (partial, final)"| Processor
    Processor --> Out["TranscriptionFrame"]
```

## Tests

None of its own yet: it needs an API key and a network. The [provider skill](../../skills/vasdk-provider/SKILL.md#tests) describes the unit and integration tests a provider should have.
