# VASDK on Android

`VASDK_ANDROID/` is a Kotlin app (Jetpack Compose) running the full VASDK pipeline natively on-device through a JNI boundary. The same core library and providers that power the desktop apps are cross-compiled per ABI by the Android Gradle Plugin's CMake integration; nothing is reimplemented in Java or Kotlin.

## How it's layered

```mermaid
flowchart TB
    subgraph Kotlin["Kotlin app (Jetpack Compose)"]
        UI["MainScreen / MainViewModel<br/>VoiceOrb · ExpectationBanner · ModelInfoOverlay"]
        Tools["tools/: camera, location, media,<br/>notifications, communication, web, …"]
    end
    subgraph Engine["VasdkEngine.kt"]
        Lifecycle["lifecycle: init → start → stop → release"]
        Listener["Listener: transcripts, LLM chunks, status,<br/>audio features, download progress, tool calls"]
    end
    subgraph JNI["JNI - vasdk_jni.cpp"]
        Bridge["builds the pipeline, bridges callbacks both ways<br/>(incl. nativeGetPromptPreview for model info)"]
    end
    subgraph Native["VASDK core + providers (C++, per-ABI)"]
        Providers["MiniAudio · SileroVAD · Parakeet · Whistle · PocketTTS · Phonon<br/>SmartTurnV3 · pyannote speaker · llama.cpp (local LLM)<br/>SonioxPP · DeepgramPP · GladiaPP (cloud STT/TTS)"]
    end

    Kotlin <--> Engine
    Engine <--> JNI
    JNI <--> Native
```

The native side is the same CMake tree as the desktop build (`externalNativeBuild` points at [`CMakeLists.txt`](../../VASDK_ANDROID/CMakeLists.txt), with `-DVASDK_BUILD_APP=OFF` so only the library and providers are compiled). It runs on [VAEngine](../reference/engine.md), as the desktop app does:
- `VasdkEngine` builds an [engine config](../reference/engine-config.md) from the settings: which providers, their keys and model folders, audio formats, always-on listening, turn-taking and the cloud LLM.
- `nativeInit` parses it (text proto, full protobuf).
- It creates the providers by id from the shared provider catalog, plus the app's own `voz` ([`VozProvider.cpp`](../../VASDK_ANDROID/app/src/main/cpp/VozProvider.cpp)).
- It runs one `Session`. Events from the session feed the `Listener`.
- It adds the local LLM (llama.cpp), the Android tools, speaker tracking and the visualiser to the session. ABIs are `arm64-v8a` and `x86_64`; `armeabi-v7a` was dropped because llama.cpp's NEON fallback collides with the NDK's `arm_neon.h` on the 32-bit target, and no realistic test device is armv7-only.

## The Kotlin API surface

`VasdkEngine` is the single entry point: a thin singleton wrapper over the native pipeline with an `init / start / stop / release` lifecycle and a `Listener` interface for everything coming back up: transcripts (partial and final), streamed LLM response chunks (with an `isExpecting` flag when the LLM's reply is itself a question awaiting a spoken answer), agent status changes, pipeline errors, VizAnim audio feature vectors for UI animation, and model download progress.

`setBackchannelConfig(enabled, pendingPlaybackMode, historyMode, tailOverlapMode, duckGainDecibels, duckDurationMs)`, called after `init` (also while running), chooses how speech over the assistant is handled ([back-channels](../reference/processors.md#back-channels-speech-over-the-agent-that-isnt-an-interruption)). The defaults are on, `DUCK`, `DROP`, `TREAT_AS_TURN` and a −12 dB duck lasting at least 2500 ms, so a "mm-hmm" ducks the assistant instead of cutting it off. `AAudioSink` applies the duck and pause on the audio callback thread.

`getModelInfo()` returns a `ModelInfo` snapshot (which model is loaded, its chat format, the current system prompt, every tool currently registered with its parameters, and a raw preview of the actual prompt/request payload) for the in-app "model info" screen:

```mermaid
sequenceDiagram
    participant UI as MainScreen
    participant VM as MainViewModel
    participant Engine as VasdkEngine.kt
    participant JNI as vasdk_jni.cpp
    participant Native as PromptTemplate / LLMTransport

    UI->>VM: request model info
    VM->>Engine: getModelInfo()
    Engine->>JNI: nativeGetPromptPreview()
    JNI->>Native: render preview (local: PromptTemplate::create,<br/>cloud: previewRequestPayload())
    Native-->>JNI: system prompt, tools, raw prompt
    JNI-->>Engine: JSON blob
    Engine-->>VM: ModelInfo(isLocal, modelName, chatFormat,<br/>systemPrompt, tools, rawPrompt)
    VM-->>UI: ModelInfoOverlay renders it
```

## An agent before the LLM

`VasdkEngine.agentAsset` names an [agent](../reference/flows.md) in the app's assets. It answers what it understands, and everything else goes on to the LLM as before. The app ships [`agents/phone/agent.txtpb`](../../VASDK_ANDROID/app/src/main/assets/agents/phone/agent.txtpb): "call mom" finds the contact by name in the phone book, asks "Call Mom?", and calls on a yes. The same conversation runs on the desktop in `ctest` (`vasdk_agent_android_phone`, cases in [`agent-tests/phone/`](../../VASDK_ANDROID/agent-tests/phone/)).

| Call | Use |
| --- | --- |
| `registerAgentTool(name) { argsJson -> resultJson }` | A tool for the agent's `call_tool` steps, written in Kotlin (`place_call` here). Declare it in the agent with no `http` (`tools { name: "place_call" }`), so lint and tests know it. |
| `registerAgentStep(paramsType) { params, turn -> stepOutcome { ... } }` | A step for `custom { [type.googleapis.com/<paramsType>] { ... } }`. It gets its params serialized (parse them with the type's class, `Points.parseFrom(params)`) and the turn's text, flow and page (`AgentTurn`), and returns a [`StepOutcome`](../../VASDK/proto/vasdk/flow/step_outcome.proto) message: values to set, texts to say, a target. |
| `setEntityValues(name, valuesJson)` | Replaces a dynamic entity's values: `ContactsEntity.values(context)` gives the phone book as `@contact` (number as the value, the name as spoken and said). |
| `events` | A `SharedFlow<Event>` of every engine event, each a [`vasdk.engine.Event`](../../VASDK/proto/vasdk/engine/events.proto): `engine.events.collect { if (it.hasAgentReply()) show(it.agentReply.text) }`. |

Register tools and steps, and set `agentAsset`, before `init()`. They run on a native thread and may block. The app's build generates Kotlin lite classes from VASDK's `.proto` files (the `com.google.protobuf` Gradle plugin, `protobuf-kotlin-lite`), the same messages the native side uses; tools take and give JSON, as their arguments are an LLM's.

The agent can also hand a topic to the LLM with a route's `target: "LLM"`; its steps are the offline answer ([With an LLM](../reference/flows.md#with-an-llm)). Not in the app yet:
- the engine config that `VasdkEngine` builds has a single LLM, so `llm_profiles` ([a model per task](../reference/engine-config.md#different-llms-for-different-tasks)) aren't in its settings;
- the session has no `Options::context`, so `{context}` in the system prompt isn't filled in.

## Triggers while the app is in the background

The agent's triggers work while the app isn't in front. The code is in [`TriggerBridge.kt`](../../VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/services/TriggerBridge.kt).

- **Schedule triggers.** While the engine runs, it checks schedule triggers every second.
  - Every minute and on `stop()`, the engine saves its trigger state. It also sets an AlarmManager alarm for the next due time: an exact alarm if the user allowed "Alarms & reminders", otherwise within Doze's window. A reboot sets the alarm again.
  - If the engine is running when the alarm rings, it says the reminder itself.
  - If it isn't, a notification asks the user to open the app, because Android 14+ doesn't let an app in the background start the microphone. The restarted engine restores the saved state and says the reminder at its first check, once.
- **Notifications.** With notification access granted, every notification another app posts, except ongoing ones, fires the agent's `notification` app event. `$trigger.payload` holds `{"package", "title", "text", "category"}`:

  ```
  triggers { name: "message" source { app_event { name: "notification" } }
             condition: "$trigger.payload.package == 'com.whatsapp'"
             action { say { text: "A message from $trigger.payload.title." } } }
  ```

Apps that keep their own state can call `Session::nextScheduledTrigger()`, `triggerState()` and `restoreTriggerState()` directly; the JNI uses the same calls.

## Tools and tool confirmation

Tool calls cross the JNI boundary too: when the LLM requests a tool, the native side calls `VasdkEngine.onToolCall` synchronously from a native worker thread, which dispatches into [`tools/`](../../VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/tools/) against real Android APIs. This is the piece that turns the assistant from a chatbot into something that can act on the device.

| Kotlin file | Native bridge header | Tools |
| --- | --- | --- |
| - | `android_tools.h` | `get_current_datetime`, `generate_random_number`, `convert_units` (pure C++, no JNI round trip) |
| `SystemTools.kt` | `system_tools.h` | `get_battery_status`, `get_device_info`, `get_wifi_info`, `get_network_status`, `get_storage_info`, `get_screen_brightness` |
| `MediaTools.kt` | `media_tools.h` | `get_volume`, `set_volume`, `set_flashlight`, `media_play`, `media_pause`, `media_play_pause` |
| `NotificationTools.kt` | `notification_tools.h` | `send_notification`, `set_alarm_or_timer` |
| `CommunicationTools.kt` | `communication_tools.h` | `get_contacts`, `get_call_logs`, `make_phone_call`\*, `get_calendar_events`, `add_calendar_event`, `send_sms`\* |
| `LocationTools.kt` | `location_tools.h` | `get_location`, `navigate_to` |
| `WebTools.kt` | `web_tools.h` | `web_search`, `fetch_url` |
| `CameraTools.kt` | `camera_tools.h` | `capture_photo`, `scan_qr_code` |

\* confirmation-gated, see below. `set_alarm_or_timer` used to be two separate tools (`set_alarm`/`set_timer`); they were merged because a small on-device model reliably mishandled the arithmetic ("an alarm in five seconds" became hour=5, minute=0). The merged tool forwards the time expression close to verbatim and does the classification/parsing deterministically in Kotlin instead of asking the model to do it.

Sensitive actions (`make_phone_call`, `send_sms`) aren't blocked behind a UI dialog - there's no one to tap it in a voice-only flow. Instead the tool itself asks the LLM to get a *spoken* confirmation, and re-validates that the confirmed call matches what was actually offered before it fires:

```mermaid
sequenceDiagram
    participant LLM
    participant Tool as CommunicationTools.kt
    participant Signal as ConfirmationSignal
    participant VM as MainViewModel
    participant UI as ExpectationBanner

    LLM->>Tool: make_phone_call / send_sms (confirmed=false)
    Tool->>Tool: cache PendingAction(tool, number, message)
    Tool->>Signal: notifyAwaiting(true)
    Signal->>VM: onAwaitingConfirmationChanged(true)
    Tool-->>LLM: requires_confirmation=true (ask the user out loud)
    LLM->>LLM: speaks the confirmation question (TTS)
    Note over VM: SYNTHESIS_COMPLETED starts the countdown
    VM->>UI: show ExpectationBanner (progress countdown)
    Note over LLM: user replies verbally, turn continues
    LLM->>Tool: same tool, confirmed=true
    Tool->>Tool: isConfirmedMatch(tool, number, message)?
    Tool->>Signal: notifyAwaiting(false)
    Signal->>VM: onAwaitingConfirmationChanged(false)
    Tool-->>LLM: action result (call placed / SMS sent)
```

If `confirmed=true` doesn't match the cached `PendingAction` (a small model can emit `confirmed=true` without ever actually asking), the tool re-issues a fresh confirmation request instead of trusting the flag. The same `ExpectationBanner` also fires for the LLM's own `isExpecting` flag on ordinary spoken questions, not just tool confirmations - `awaitingResponse` in `MainViewModel`'s UI state is the single source of truth either way.

## Voice orb and waveform visualization

`VoiceOrb.kt` renders a ChatGPT-style animated orb - five per-band blobs (bass through treble) plus three additively-blended combination blobs - driven by `level` (overall loudness) and `bars` (per-band magnitudes), with a `thinking` mode that swaps the audio-driven energy for a metronome pulse while the LLM is still generating. It uses a blue palette while the mic is live and a red palette while the assistant is speaking. The older five-bar `WaveformIndicator` is still in the codebase but disabled (`USE_WAVEFORM_BARS = false`) in favor of the orb.

`MainViewModel` tracks *which* stream currently owns the waveform, since mic and TTS-output magnitudes differ by orders of magnitude and can't share smoothing state:

```mermaid
stateDiagram-v2
    [*] --> NONE
    NONE --> MIC: onSpeechStarted
    NONE --> SYNTH: agent starts speaking
    MIC --> NONE: hideWaveform() (~210ms fade)
    SYNTH --> NONE: hideWaveform() (~210ms fade)
    MIC --> SYNTH: agent starts speaking
    SYNTH --> MIC: onSpeechStarted

    state MIC {
        [*] --> FALLBACK_M
        FALLBACK_M --> DATA_DRIVEN_M: first onAudioFeatures(MIC, ...)
    }
    state SYNTH {
        [*] --> FALLBACK_S
        FALLBACK_S --> DATA_DRIVEN_S: first onAudioFeatures(SYNTH, ...)
    }
```

`showWaveform(source)` resets to `FALLBACK` (an idle animation) whenever the owning source changes, and `onAudioFeatures(source, features)` only applies incoming native audio features - promoting the mode to `DATA_DRIVEN` - when the frame's source still matches the current owner, so a late frame from the stream that just lost ownership can't corrupt the other one's display.

## Settings and provider switching

`Header.kt` exposes four actions beyond the status pill: animation test (`GraphicEq`), model info (`Info`), new conversation (`RestartAlt`), and settings (`Settings`) - the LLM mode toggle that used to live directly in the header was folded into the settings overlay instead. `SettingsOverlay.kt` lets you swap STT/TTS/LLM providers at runtime, no rebuild required:

- **Speech-to-Text** - `SttProvider`: `PARAKEET` · `WHISTLE` (on-device, arm64 only) · `VOZ` (on-device, through the Desert Ant Labs Kotlin SDK `ai.desertant:voz`, which downloads its own model; Settings shows the attribution its license requires) · `SONIOX` · `DEEPGRAM` · `GLADIA` (cloud), plus a streaming/one-shot toggle.
- **Text-to-Speech** - `TtsProvider`: `POCKET` · `PHONON` (on-device, `gradium/phonon-31m-en`, CC BY-NC 4.0) · `SONIOX` · `DEEPGRAM` (cloud).
- **Language Model** - on-device/cloud toggle; on-device exposes the same presets as `VasdkEngine.LOCAL_LLM_MODELS`.

"Apply Config" tears down and re-initializes the native pipeline with the newly selected providers. `sttModelDir`/`ttsModelDir` (`parakeet/`, `whistle/` or `voz/`, `pocket_tts/` or `phonon_tts/` under `cacheDir`) are only meaningful for the currently selected on-device provider - the native side ignores them when a cloud provider is active instead. Choices persist across restarts via `ProviderPreferences`.

## Local LLM on-device

The app runs GGUF models on-device through llama.cpp. Models aren't bundled in the APK; `ModelDownloadService` (a foreground service with notification progress) downloads them on first run, driven by `VasdkEngine`'s download callbacks, alongside the on-device STT (Parakeet/Whistle) and TTS (PocketTTS/Phonon) model files and the pyannote speaker-embedding model (`pyannote/`, always on: `SpeakerIdentificationProcessor` logs SAME/NEW speaker per utterance) - VAD (Silero) and turn detection (SmartTurn v3) ship embedded as raw resources instead and are extracted to disk at `init()`. The default model is `LFM2_5Meta` (LFM2.5 230M, chat format `"lfm2.5"`); `Llama-3.2-1B-Instruct` (`"llama3"`), `Qwen2.5-1.5B-Instruct` (`"chatml"`), and `google_gemma-3-1b-it` (`"gemma"`) are also declared as presets in `VasdkEngine.Companion`. The engine needs to know the model's chat format so the native side renders prompts and tool descriptions correctly for that model; cloud mode (`useLocalLLM = false`) always reports `"openai"` regardless of the actual backing model.

## Building

Open `VASDK_ANDROID/` in Android Studio, or:

```bash
cd VASDK_ANDROID
./gradlew assembleRelease
```

Requirements: compileSdk 35, minSdk 33, NDK 28 (NDK 26 crashes compiling protobuf 36), CMake 3.22+. Code is generated from the `.proto` files with the build machine's `protoc` 36.2 (Homebrew's, say); without one, the build downloads it. The first build downloads the native dependency graph (ONNX Runtime, llama.cpp, sentencepiece, curl, and the rest) per ABI, so expect it to take a while.

For a cloud LLM instead of (or alongside) the local one, provide credentials at build time; they're injected into `BuildConfig`:

- `.env.local` at the repository root (`LLM_API_KEY`, `LLM_ENDPOINT`, `LLM_MODEL`), or
- `local.properties` in `VASDK_ANDROID/` (`llm.apiKey`, `llm.endpoint`, `llm.model`).

`LLM_REASONING_EFFORT` (or `llm.reasoningEffort`) becomes the config's `llm.effort`. Set it to `none` so a thinking model answers at once.

To use the cloud LLM, turn **On-device** off under Language model in Settings.

**An LLM server on the development machine**, such as LM Studio at `LLM_ENDPOINT=http://127.0.0.1:1234/v1/chat/completions`: on the device, 127.0.0.1 is the device itself. Tunnel the port, on an emulator or a USB phone:

```bash
adb reverse tcp:1234 tcp:1234
```

Run it again after the emulator or adb restarts.

**Typing instead of talking**, for testing: [`TestInputReceiver`](../../VASDK_ANDROID/app/src/main/kotlin/com/vasdk/android/services/TestInputReceiver.kt) takes a user turn from adb, as if it was spoken. The reply is spoken and logged:

```bash
adb shell am broadcast -a com.vasdk.android.SAY --es text "what's the capital of France" \
  -n com.vasdk.android/.services.TestInputReceiver
adb logcat | grep -E "LLMProcessor|Synthesizing"
```

Only adb can send it: the receiver requires `android.permission.DUMP`, which apps can't hold.

For the cloud STT/TTS providers (Soniox, Deepgram, Gladia) selectable from the in-app Settings overlay, add their keys the same way:

- `.env.local` (`SONIOX_API_KEY`, `DEEPGRAM_API_KEY`, `GLADIA_API_KEY`), or
- `local.properties` (`soniox.apiKey`, `deepgram.apiKey`, `gladia.apiKey`).

All of these are optional at build time - the providers are compiled in regardless, and a missing key only matters if that provider is actually selected at runtime. `.env.local` wins when both are present. Neither file is committed.

### Tests on a device

The native suite, the same `ctest` cases as on the desktop, runs on a device or emulator through adb:

1. Build the tests for the device's ABI with the NDK, then run them:

   ```bash
   cmake -S VASDK_ANDROID -B build/android-x86_64 -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
     -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release -DVASDK_BUILD_TESTS=ON
   cmake --build build/android-x86_64
   ANDROID_NDK=... sh VASDK_ANDROID/run-tests-on-device.sh build/android-x86_64 build/VASDK_3P_IMPL/models
   ```

2. [`run-tests-on-device.sh`](../../VASDK_ANDROID/run-tests-on-device.sh) does the rest:
   - It pushes each test and runs it as ctest would. Golden files, entity packs and test commands keep the paths they had at build time.
   - With a models folder, the offline spoken conversation runs. It uses Silero, SmartTurn, Pocket and Phonon, with Whistle for STT where its engine is built (arm64), or Parakeet otherwise.
   - It deletes each binary once its test has run: they're statically linked, and an emulator fills up.
   - On an x86_64 emulator (API 33, 6 GB), all 52 cases pass or skip. The skips are tests whose models weren't pushed: MOSS, a GGUF.
