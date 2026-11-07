# VASDK on Android

A Kotlin app (Jetpack Compose) that runs the full VASDK pipeline on the phone through JNI: local speech recognition, voices and LLM, device tools with spoken confirmation, an agent before the LLM, and triggers that wake it in the background.

**The full guide: [docs/apps/android.md](../docs/apps/android.md).**

```bash
cd VASDK_ANDROID
./gradlew assembleRelease          # compileSdk 35, minSdk 33, NDK 28, CMake 3.22+
./run-tests-on-device.sh           # the native suite on a device or an emulator
```

A cloud LLM's credentials come from `.env.local` at the repository root (`LLM_API_KEY`, `LLM_ENDPOINT`, `LLM_MODEL`) or from `local.properties` (`llm.apiKey`, `llm.endpoint`, `llm.model`). For an LLM server on your computer, tunnel its port: `adb reverse tcp:1234 tcp:1234`.

| Path | |
| --- | --- |
| [`app/src/main/kotlin/`](app/src/main/kotlin/) | The app: `VasdkEngine`, the UI, the device tools, the trigger bridge |
| [`app/src/main/cpp/`](app/src/main/cpp/) | The JNI layer over the native engine |
| [`app/src/main/assets/agents/`](app/src/main/assets/agents/) | The example agent (phone calls) |
| [`agent-tests/`](agent-tests/) | Its conversations, run on the desktop in `ctest` |
