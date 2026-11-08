# Building VASDK

*How to configure and build the core library, the providers and the example apps. The root [README](../README.md#quick-start) has the short version; [Providers](provider-catalog.md) covers each provider's options.*

## Prerequisites

- CMake 3.16+ and a C++17 compiler.
- Git on your `PATH` - needed both to clone the repo and by CMake's `FetchContent` to pull every C++ dependency at configure time.
- libcurl, for every HTTP-based transport (OpenAI/Anthropic/OpenRouter LLM calls, Gladia/Deepgram/Cartesia/Fish Audio cloud STT/TTS). No install is needed: configure uses an installed libcurl when CMake finds one (system package, Homebrew, or a vcpkg toolchain if you pass one), and otherwise builds curl 8.21 from source as a static library. On Windows that build uses Schannel, the OS TLS stack, so neither vcpkg nor OpenSSL is required. Pass `-DVASDK_FETCH_CURL=ON` to always build it from source, or `-DVASDK_ENABLE_HTTP_TRANSPORT=OFF` to drop it (which also disables every cloud provider, leaving only local/offline ones). On Linux/macOS a from-source build picks up OpenSSL (`apt install libssl-dev` / `brew install openssl`). Note that CMake also reads `CMAKE_TOOLCHAIN_FILE` from the environment, so a machine-wide vcpkg setting applies to every new build directory.
- protobuf, for engine config files. Configure uses an installed one (`brew install protobuf`, `apt install libprotobuf-dev protobuf-compiler`, or vcpkg) and otherwise builds it from source, which makes the first configure slower.
- Python 3.8+ for the Python binding, the Python app and their tests, with the packages in [`VASDK_PYTHON_APP/pyproject.toml`](../VASDK_PYTHON_APP/pyproject.toml) (uv) or [`requirements.txt`](../VASDK_PYTHON_APP/requirements.txt) (pip); pass `-DPython3_EXECUTABLE=` to test with that environment ([docs/apps/python.md](apps/python.md#testing)). The checkers that ctest runs (`skills/check_skills.py`, `VASDK_3P_IMPL/check_rules.py`) use the standard library only. The two golden generators (`VASDK_3P_IMPL/SmartTurnV3/tests/gen_features_golden.py`, `VASDK_3P_IMPL/PyannoteSpeaker/tests/gen_fbank_golden.py`) declare their packages inline: `uv run <script>` installs them.
- A network connection for the first configure - everything else (C++ deps and model files) is fetched for you, and re-configures skip anything already downloaded.

## Build options

```bash
# Core library only, no provider implementations
cmake -B build -S . -DVASDK_BUILD_THIRDPARTY_IMPL=OFF

# Providers as shared libs instead of static
cmake -B build -S . -DVASDK_THIRDPARTY_SHARED_LIBS=ON

# Terminal UI for VASDK_Pipeline (FTXUI)
cmake -B build -S . -DVASDK_BUILD_TUI=ON

# The language bindings, off by default: Python's tests and app, the Kotlin JVM library and its
# tests (needs Gradle and a JDK 17+). Both load VASDK at run time, as a shared library. The binding
# parity check (vasdk_binding_parity) is registered only with one on, for C and the bindings on;
# configure warns that it isn't when both are off.
cmake -B build -S . -DVASDK_BUILD_PYTHON_BINDING=ON -DVASDK_BUILD_KOTLIN_BINDING=ON

# Metrics exporters (all default OFF, pick what your monitoring stack speaks)
cmake -B build -S . -DVASDK_METRICS_PROMETHEUS=ON -DVASDK_METRICS_STATSD=ON

# WebRTC transport provider + browser demo page (libdatachannel, Opus; default OFF)
cmake -B build -S . -DVASDK_BUILD_LIBDATACHANNEL_WEBRTC=ON
```

With multi-config generators (Visual Studio, Xcode) the binaries land one level deeper, under a `Release/` subdirectory (e.g. `./build/VASDK_APP/Release/VASDK_Pipeline`). The apps print an error and exit if they can't find the models directory or a required provider wasn't built; check the CMake configure output, which lists exactly which providers were enabled.

## Model files

Model files are downloaded automatically during CMake configure, into `build/VASDK_3P_IMPL/models/`. Each enabled provider pulls its own files from the model's upstream release (HuggingFace for most) and skips anything already on disk, so re-configuring is cheap. At runtime, both showcase apps locate that directory by searching for `VASDK_3P_IMPL/models` upward from the executable's location, so the default build layout just works.

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
