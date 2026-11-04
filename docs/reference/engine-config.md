# Engine config files

Source of truth: [`engine_config.proto`](../../VASDK/proto/vasdk/engine/engine_config.proto), [`ProtoFileLoader.h`](../../VASDK/include/VASDK/engine/ProtoFileLoader.h), [`ProviderRegistry.h`](../../VASDK/include/VASDK/engine/ProviderRegistry.h), [`options.proto`](../../VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto).

An engine config picks the VAD, STT, TTS and turn-detection providers and the LLM settings at run time, so switching from Whistle to Parakeet, or from PocketTTS to Cartesia, is a file edit instead of a rebuild. The schema is a protobuf message (`vasdk.engine.EngineConfig`); you write it as text proto.

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

Run it with `VASDK_Pipeline --config my-engine.txtpb`, or build an assistant from it in your own app with [`VAEngine`](engine.md). A commented example ships in [`VASDK_APP/config/engine.txtpb`](../../VASDK_APP/config/engine.txtpb).

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

- `id`: which provider to create. When empty, the first one built into the binary is used, in this order. VAD: `silero`. STT: `gradium`, `soniox`, `deepgram`, `cartesia`, `fishaudio`, `whistle`, `moonshine`, `parakeet`. TTS: `gradium`, `soniox`, `supertonic`, `deepgram`, `cartesia`, `fishaudio`, `moss`, `phonon`, `pocket`. Turn: `smartturn-v3`; with no turn model built in, the VAD's silence ends the turn. This order matches what VASDK_Pipeline picked before config files existed, so running without `--config` behaves as before.
- `options`: the provider's own options message, written with the `Any` syntax `[type.googleapis.com/<message>] { ... }`. Every field is optional. Unset fields fall back to the provider's environment variable (`SONIOX_API_KEY`, `PHONON_TTS_VOICE`, …), then to its default.

  | Message | Providers | Fields |
  | --- | --- | --- |
  | `vasdk.providers.CloudOptions` | cloud STT/TTS (`gladia`: STT only) | `api_key`, `voice_id`, `ca_bundle_path` (certificates to trust; Android's TLS can't read the system store) |
  | `vasdk.providers.SileroOptions` | `silero` | `model_path`, `threshold` (speech probability a frame needs, 0–1; default 0.75) |
  | `vasdk.providers.LocalSttOptions` | `whistle`, `moonshine`, `parakeet` | `model_dir`, `idle_timeout_ms`, `partial_interval_ms`, `language` |
  | `vasdk.providers.LocalTtsOptions` | `supertonic`, `moss`, `phonon`, `pocket` | `model_dir`, `voice`, `language`, `steps`, `prebuffer_ms` (pocket, moss: audio made before playback starts, against stalls on slow phones) |
  | `vasdk.providers.SmartTurnOptions` | `smartturn-v3` | `model_path` |

- `fallback`: ids to try, in order, when `id` is not built into the binary or fails to start (a missing model file, for example), and to switch to when the network drops ([When the network drops](engine.md#when-the-network-drops)). `options` apply to `id`; a fallback gets its `fallback_options` entry (`fallback_options { key: "parakeet" value { [type.googleapis.com/vasdk.providers.LocalSttOptions] { model_dir: "models/parakeet" } } }`), or starts with its defaults. The log shows which provider was used and why the others were skipped.

## Activation

```textproto
activation { mode: WAKE_WORD follow_up { listen_ms: 8000 } }
providers { wake_word { id: "my-wake-word" } }
```

`mode` sets when the assistant listens:

| Mode | Before a conversation | After each reply, when the follow-up window closes |
| --- | --- | --- |
| `ALWAYS_ON` (default) | always listening | still listening |
| `WAKE_WORD` | asleep until the wake phrase; needs a `providers.wake_word` | asleep |
| `PUSH_TO_TALK` | listens while the app holds the button (`Session::pushToTalk`) | asleep |
| `HYBRID` | wake phrase or button | asleep |

`follow_up.listen_ms` keeps the mic open after **every** reply, so the user can answer without the wake phrase or button. The window starts once the reply has finished playing, or was interrupted. Speech that starts in it is a new turn. Unset: 8000 ms; `0` closes it at once; `-1` never closes it. `Session::setFollowUpListenMs()` changes it at run time, for example from a settings screen. An agent can set its own, per page or per reply ([Expected input](flows.md#expected-input)). Apps get `listening_window` events to drive a mic indicator.

No wake-word provider ships yet (milestone 28), so `WAKE_WORD` and `HYBRID` need one registered under `audio::WakeWordProvider`, as [`activation_test.cpp`](../../VASDK/tests/activation_test.cpp) does.

## Understanding

`providers { sentence_encoder { id: "minilm" options { [type.googleapis.com/vasdk.providers.MiniLmOptions] { model_dir: "models/minilm" } } } }` lets agents understand paraphrases after their rules, and answer knowledge questions by meaning (see [Understanding](flows.md#understanding)). Without it, agents understand by their examples alone.

## Turn-taking

```textproto
turn_taking { preset: SNAPPY streaming_transcription: true }
```

`preset` sets how quickly the assistant decides the user has finished speaking:

| Preset | Speech to start / minimum speech / pause within speech / silence to end (ms) | For |
| --- | --- | --- |
| `BALANCED` (default) | 250 / 250 / 500 / 250 | a desktop assistant |
| `SNAPPY` | 150 / 150 / 200 / 150 | phone calls (VASDK_CallCenter) |
| `PATIENT` | 250 / 400 / 500 / 600 | people who stop to think |

In every preset, interrupting the assistant needs only 150 ms of speech: echo cancellation turns the user's voice down while the assistant talks.

`streaming_transcription` runs the STT while the user speaks, so partial results arrive before the turn ends. Speculative firing needs it.

Any timing can be set on its own; the others keep the preset's values:

```textproto
turn_taking {
  preset: BALANCED
  voice_activity { end_silence_ms: 800 pause_ms: 700 }    # let people pause longer
  backchannel { pending_playback: PAUSE commit_after_ms: 400 interrupt_words: ["stop", "wait", "cancel"] }
  interrupt_timeout_ms: 800
}
```

| `voice_activity` field | Meaning | `VADConfig` field |
| --- | --- | --- |
| `start_ms` | voice this long before speech may start | `vadThresholdMs` |
| `min_speech_ms` | speech this long before it counts; shorter blips are dropped | `minSpeechDurationMs` |
| `pause_ms` | silence this long inside speech is a pause | `maxSpeechPauseDurationMs` |
| `end_silence_ms` | after a pause, silence this long ends the speech | `maxSpeechEndDurationMs` |
| `resume_silent_frames` | silent frames while resuming before it's a pause again (default 3) | `resumingSilenceFramesToPause` |
| `barge_in_min_speech_ms` | `min_speech_ms` while the assistant speaks (default 150; 0: same as `min_speech_ms`) | `bargeInMinSpeechDurationMs` |
| `lookback_factor` | audio kept from before the detected start, as a multiple of the start window (default 3 for `BALANCED`, 5 otherwise) | `VADFrameProcessor` constructor |

`backchannel` decides whether speech over the assistant is a back-channel ("yeah", "mm-hmm") or an interruption. Each field replaces the same value in `Session::Options::backchannel` (`pipeline::BackchannelConfig`), so a value set in the file wins over the app's: `enabled`, `pending_playback` (`DUCK`, `PAUSE`, `CONTINUE`), `history` (`DROP`, `APPEND_TO_HISTORY`), `tail_overlap` (`TREAT_AS_TURN`, `CLASSIFY`), `duck_gain_db`, `duck_ms`, `commit_after_ms`, `final_transcription_wait_ms`, `tail_overlap_ms`, `max_backchannel_words`, `min_interrupt_confidence`, `low_confidence_max_words`, `phrases` and `interrupt_words`. The two lists replace the built-in ones when set.

`interrupt_timeout_ms` is how long the assistant waits in `INTERRUPTED` for the TTS to confirm it stopped before listening anyway (default 500).

## Audio

```textproto
audio {
  input  { sample_rate: 16000 encoding: PCM_16 channels: 1 }   # what the VAD, turn model, wake word and STT receive
  output { sample_rate: 24000 encoding: PCM_16 channels: 1 }   # what the TTS produces
  frame_ms: 100                                                 # the grid VAD and turn decisions run on
}
```

These replace `Runtime::Options` / `VAEngine::Options` `inputAudio`, `outputAudio` and `frameSize`, value by value. Encodings: `PCM_16` (default), `PCM_8`, `PCM_32`, `FLOAT_32`. Providers resample from their own model rates to these, and the transport is configured with them. Changing only the input rate keeps the frame's duration. Out-of-range values (a rate outside 8000–192000 Hz, 0 channels, `frame_ms` outside 10–1000) fail `create()` with the field's name.

An app that opens its audio devices before creating the engine calls `va::engine::applyAudioConfig(config.audio(), ...)` first, so the devices use the same formats; VASDK_Pipeline does.

## LLM

```textproto
llm {
  endpoint: "${LLM_ENDPOINT}" model: "${LLM_MODEL}" api_key: "${LLM_API_KEY}"
  history: 12
  speculative_firing: true speculative_min_words: 3
  tool_filler_phrases: ["One moment.", "Let me check that."]
}
```

| Field | Meaning |
| --- | --- |
| `endpoint`, `model`, `api_key` | An OpenAI-compatible chat endpoint. Without an endpoint (and no LLM passed in code), there is no LLM. |
| `history` | Messages kept in the conversation; default 10. |
| `speculative_firing`, `speculative_min_words` | Start the request on the partial at a pause of at least `speculative_min_words` words (default 3); the reply is held until the final transcript matches. Needs `streaming_transcription`. |
| `tool_filler_phrases` | Said in turn while a tool runs, so the caller doesn't hear silence. |

These apply also when the app passes its own LLM transport in code, as VASDK_CallCenter does with `--llm-endpoint`.

`models_dir` sets the base directory for local model files. When it is unset, VASDK_Pipeline uses the `models/` folder it finds next to the executable. A relative path is resolved from the current directory.

## Security

What agents' tools may reach. Secrets live here, never in agent files, which fail to load with one.

```textproto
security {
  http_allow: ["https://api.acme.example/*"]
  max_response_kb: 256
  timeout_ms: 8000
  secrets { key: "acme_api" value: "${ACME_API_KEY}" }
}
```

| Field | Meaning |
| --- | --- |
| `http_allow` | URLs an agent's HTTP tools may call; `*` matches anything. End a host with `/*`: `https://api.acme.example*` would also match `https://api.acme.example.evil.example`. None listed: no HTTP tool works. |
| `max_response_kb` | A longer reply fails the call; default 256. |
| `timeout_ms` | A tool, C++ or HTTP, still running after this fails; default 8000. |
| `secrets` | Read by tool headers as `$secrets.<name>`. Use `${ENV}` here so the values stay out of the file too. |

See [Tools and security](flows.md#tools-and-security).

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
