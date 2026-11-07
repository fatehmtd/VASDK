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

Build and test it against a VASDK build (it needs VASDK as a shared library):

```bash
cmake -B build -S . -DVASDK_BUILD_TESTS=ON -DVASDK_BUILD_KOTLIN_BINDING=ON && cmake --build build -j
ctest --test-dir build -R vasdk_kotlin
# or by hand
VASDK_LIBRARY=$PWD/../../build/VASDK/libVASDK.dylib gradle test
```

Its dependencies, and their licenses (all in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)): JNA (Apache 2.0 / LGPL 2.1), protobuf-kotlin-lite (BSD 3-Clause), kotlinx.coroutines and kotlinx.serialization (Apache 2.0).
