# vasdk for Kotlin

VASDK's agents, spoken sessions and providers from Kotlin, on the JVM (macOS, Linux, Windows) and on Android (the `:vasdk` AAR in [`VASDK_ANDROID/vasdk`](../../VASDK_ANDROID/vasdk/)), over the C API by JNA. The reference is [docs/reference/kotlin.md](../../docs/reference/kotlin.md).

```kotlin
val agent = Agent.load("VASDK_APP/agents/garage-menu/agent.txtpb")
agent.conversation().use { println(it.start().text) }

Session("engine.txtpb", Providers(), agent).use { session ->
    session.onEvent { println(it) }
    session.start()
    session.injectText("book an oil change")
}
```

The config can also be built in code, each provider's settings its own message, packed with `toAny()`:

```kotlin
val config = vasdk.engine.engineConfig {
    providers = vasdk.engine.providers {
        vad = vasdk.engine.providerSpec { id = "silero" }
        stt = vasdk.engine.providerSpec { id = "whistle" }
        tts = vasdk.engine.providerSpec { id = "phonon"; options = vasdk.providers.phononOptions { voice = "Zoey"; temperature = 0.4f }.toAny() }
    }
}
Session(config, Providers(), agent).use { it.start() }   // Runtime(config, ...) too
```

The model files local providers load come from the model store, which checks their SHA-256 and keeps them where a session with no models directory looks ([Models](../../docs/models.md)). On Android give it the app's files directory, and pull off the main thread:

```kotlin
Models(directory = filesDir.resolve("models").path).use { models ->
    for (id in models.forConfig("engine.txtpb")) models.pull(id) { _, _, done, total -> println("$done/$total"); true }
}
```

Build and test it against a VASDK build (it loads the C library, `vasdk_c`):

```bash
cmake -B build -S . -DVASDK_BUILD_TESTS=ON -DVASDK_BUILD_KOTLIN_BINDING=ON && cmake --build build -j
ctest --test-dir build -R vasdk_kotlin
# or by hand
VASDK_LIBRARY=$PWD/../../build/cmake/c_library/libvasdk_c.dylib gradle test
```

Its dependencies, and their licenses (all in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)): JNA (Apache 2.0 / LGPL 2.1), protobuf-kotlin-lite (BSD 3-Clause), kotlinx.coroutines and kotlinx.serialization (Apache 2.0).
