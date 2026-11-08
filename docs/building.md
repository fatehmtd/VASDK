# Building VASDK

*How to configure and build the core library, the providers and the example apps. The root [README](../README.md#quick-start) has the short version; [Providers](provider-catalog.md) covers each provider's options; [Using VASDK in your project](integrating.md) covers depending on VASDK from another project.*

## Prerequisites

- CMake 3.21+ (3.24+ for the C library's whole-archive link; older ones use per-platform linker flags) and a C++17 compiler.
- Git on your `PATH` - needed both to clone the repo and by CMake's `FetchContent` to pull every C++ dependency at configure time.
- libcurl, for every HTTP-based transport (OpenAI/Anthropic/OpenRouter LLM calls, Gladia/Deepgram/Cartesia/Fish Audio cloud STT/TTS). No install is needed: configure uses an installed libcurl when CMake finds one (system package, Homebrew, or a vcpkg toolchain if you pass one), and otherwise builds curl 8.21 from source as a static library. On Windows that build uses Schannel, the OS TLS stack, so neither vcpkg nor OpenSSL is required. Pass `-DVASDK_FETCH_CURL=ON` to always build it from source, or `-DVASDK_ENABLE_HTTP_TRANSPORT=OFF` to drop it (which also disables every cloud provider, leaving only local/offline ones). On Linux/macOS a from-source build picks up OpenSSL (`apt install libssl-dev` / `brew install openssl`). Note that CMake also reads `CMAKE_TOOLCHAIN_FILE` from the environment, so a machine-wide vcpkg setting applies to every new build directory.
- protobuf, for engine config files. Configure uses an installed one (`brew install protobuf`, `apt install libprotobuf-dev protobuf-compiler`, or vcpkg) and otherwise builds it from source, which makes the first configure slower.
- Python 3.8+ for the Python binding, the Python app and their tests, with the packages in [`VASDK_PYTHON_APP/pyproject.toml`](../VASDK_PYTHON_APP/pyproject.toml) (uv) or [`requirements.txt`](../VASDK_PYTHON_APP/requirements.txt) (pip); pass `-DPython3_EXECUTABLE=` to test with that environment ([docs/apps/python.md](apps/python.md#testing)). The checkers that ctest runs (`skills/check_skills.py`, `VASDK_3P_IMPL/check_rules.py`) use the standard library only. The two golden generators (`VASDK_3P_IMPL/SmartTurnV3/tests/gen_features_golden.py`, `VASDK_3P_IMPL/PyannoteSpeaker/tests/gen_fbank_golden.py`) declare their packages inline: `uv run <script>` installs them.
- A network connection for the first configure - everything else (C++ deps and model files) is fetched for you, and re-configures skip anything already downloaded.

## What a build makes

A plain `cmake -B build -S .` builds the SDK:

- **The C++ libraries.** `VASDK` and the provider libraries, static.
- **The C library, `vasdk_c`.** One shared library with the C API and every provider built, exporting nothing but `vasdk_*`. The bindings load it, and `cmake --install` installs it ([Using VASDK in your project](integrating.md)).
- **`vasdk_agentc`.** It checks and tests agent files.

The sample apps are optional and off by default. Turn them all on with `VASDK_BUILD_APPS`, or one at a time:

```bash
cmake -B build -S . -DVASDK_BUILD_APPS=ON          # VASDK_Pipeline, VASDK_CallCenter, VASDK_Car
cmake -B build -S . -DVASDK_BUILD_APP=ON           # the pipeline app alone (VASDK_BUILD_CALLCENTER, VASDK_BUILD_CAR)
```

The tests run with `-DVASDK_BUILD_TESTS=ON`; the garage agent's suites live in `VASDK_APP`, so a full test run also turns on the apps (`-DVASDK_BUILD_APPS=ON`).

### Presets

[`CMakePresets.json`](../CMakePresets.json) names the usual builds. Each one builds into `build/<preset>`:

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # tests and apps
cmake --preset apps && cmake --build --preset apps                       # the sample apps
cmake --preset release && cmake --build --preset release                 # the C library as packages ship it
```

| Preset | What |
| --- | --- |
| `dev` | Tests and apps, RelWithDebInfo |
| `apps` | The sample apps, Release |
| `ci` | Tests, apps and the Python binding: what CI runs |
| `release`, `release-core`, `release-full` | The C library in a flavor ([Flavors](integrating.md#flavors)), no models, without AVX2 so it runs on any x86_64 CPU |
| `system-deps` | `release` with every dependency from `find_package` (`VASDK_DEPENDENCY_SOURCE=SYSTEM`) |

## Build options

```bash
# Core library only, no provider implementations
cmake -B build -S . -DVASDK_BUILD_THIRDPARTY_IMPL=OFF

# A named set of providers: core (none), default, full (docs/integrating.md#flavors)
cmake -B build -S . -DVASDK_FLAVOR=full

# Providers as shared libs instead of static
cmake -B build -S . -DVASDK_THIRDPARTY_SHARED_LIBS=ON

# Terminal UI for VASDK_Pipeline (FTXUI)
cmake -B build -S . -DVASDK_BUILD_TUI=ON

# The language bindings, off by default: Python's tests and app, the Kotlin JVM library and its
# tests (needs Gradle and a JDK 17+). Both load the C library, vasdk_c, at run time. The binding
# parity check (vasdk_binding_parity) is registered only with one on, for C and the bindings on;
# configure warns that it isn't when both are off.
cmake -B build -S . -DVASDK_BUILD_PYTHON_BINDING=ON -DVASDK_BUILD_KOTLIN_BINDING=ON

# Metrics exporters (all default OFF, pick what your monitoring stack speaks)
cmake -B build -S . -DVASDK_METRICS_PROMETHEUS=ON -DVASDK_METRICS_STATSD=ON

# WebRTC transport provider + browser demo page (libdatachannel, Opus; default OFF)
cmake -B build -S . -DVASDK_BUILD_LIBDATACHANNEL_WEBRTC=ON

# The C++ core as a shared library (needs a shared protobuf; the C library is then not built,
# since the shared core exports the C API itself)
cmake -B build -S . -DVASDK_SHARED=ON

# A binary for any x86_64 CPU: no AVX2/FMA (the release presets do this)
cmake -B build -S . -DVASDK_ENABLE_AVX2=OFF
```

### Dependencies

The C++ dependencies (spdlog, nlohmann/json, Highway, CLI11, ONNX Runtime) come from where `VASDK_DEPENDENCY_SOURCE` says ([`cmake/VASDKDependencies.cmake`](../cmake/VASDKDependencies.cmake)):

| Value | Where from |
| --- | --- |
| `FETCH` (default) | Downloaded at configure time, at a pinned tag, commit or SHA-256 |
| `SYSTEM` | `find_package` only, never a download: vcpkg, Conan, Homebrew, distro packages. A missing one fails the configure |
| `AUTO` | `find_package`, else downloaded |

Every mode reuses a target the outer project already defines (`spdlog::spdlog`, …). protobuf and libcurl are always taken from an installed copy when there is one (above). A provider's own client library (llama.cpp, whisper.cpp, the cloud clients) is fetched at a pinned tag or commit.

```bash
cmake -B build -S . -DVASDK_DEPENDENCY_SOURCE=SYSTEM -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

With multi-config generators (Visual Studio, Xcode) the binaries land one level deeper, under a `Release/` subdirectory (e.g. `./build/VASDK_APP/Release/VASDK_Pipeline`). The apps print an error and exit if they can't find the models directory or a required provider wasn't built; check the CMake configure output, which lists exactly which providers were enabled.

## Model files

Model files are downloaded automatically during CMake configure, into `build/VASDK_3P_IMPL/models/`, when VASDK is the top-level project. `-DVASDK_DOWNLOAD_MODELS=OFF` turns every download off. That is the default for a project that embeds VASDK and for the release presets, which get their models at run time instead ([Using VASDK in your project, Models](integrating.md#models)). Each enabled provider pulls its own files from the model's upstream release (HuggingFace for most) and skips anything already on disk, so re-configuring is cheap. At runtime, both showcase apps locate that directory by searching for `VASDK_3P_IMPL/models` upward from the executable's location, so the default build layout just works.

An enabled provider downloads its models by default. Several also have a `VASDK_DOWNLOAD_*` switch to skip that download. The exception is llama.cpp, whose model download is off by default even when the provider is built:

```bash
# Skip a model download (per provider)
cmake -B build -S . -DVASDK_DOWNLOAD_PHONON_TTS_MODEL=OFF

# Build llama.cpp and download its model (default: LFM2.5 1.2B Instruct Q4_K_M)
cmake -B build -S . -DVASDK_BUILD_LLAMACPP=ON -DVASDK_DOWNLOAD_LLAMACPP_MODEL=ON

# The GLiNER entity model (off by default; 183 MB, or 349 MB with -DVASDK_GLINER_MODEL=multi)
cmake -B build -S . -DVASDK_BUILD_GLINER_NER=ON

# Swap the default llama.cpp model
cmake -B build -S . -DVASDK_LLAMACPP_MODEL_URL=https://huggingface.co/.../model.gguf
```

Per-provider model options (Phonon's gated checkpoint, MOSS-TTS-Nano, Whistle, Moonshine, Voz, GLiNER) are in [Providers, per-provider notes](provider-catalog.md#per-provider-notes).

For air-gapped builds, turn the downloads off and place the files in `build/VASDK_3P_IMPL/models/` yourself; each `VASDK_3P_IMPL/cmake/Download*.cmake` script documents the exact filenames its provider expects.
