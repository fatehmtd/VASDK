# Third-Party Notices

VASDK itself is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)). The models it runs are not. This file lists every model the project uses, where it comes from, and the license it is distributed under.

Two model files are bundled directly in this repository (for the Android app). Everything else is downloaded from its upstream source at CMake configure time by the scripts in `VASDK_3P_IMPL/cmake/`, or at run time by the model store from the pinned list in `VASDK_3P_IMPL/models/manifest.json` ([Models](docs/models.md)); The Android app downloads its models at runtime from the same upstream sources. Neither path redistributes those files here, but their licenses still govern how you may use and ship them.

Library and provider dependencies (spdlog, nlohmann/json, llama.cpp, whisper.cpp, ONNX Runtime, CLI11, and the rest) are fetched from their upstream repositories at build time under their own licenses and are not redistributed in this repository. They're itemized in [Provider and library dependencies](#provider-and-library-dependencies) below.

## Model summary

| Model | Pipeline stage | Source | License |
| --- | --- | --- | --- |
| Silero VAD | Voice activity detection | [snakers4/silero-vad](https://github.com/snakers4/silero-vad) | MIT |
| Smart Turn v3 | Turn detection | [pipecat-ai/smart-turn-v3](https://huggingface.co/pipecat-ai/smart-turn-v3) | BSD 2-Clause |
| WeSpeaker ResNet34-LM (ONNX) | Speaker identification (`VASDK_BUILD_PYANNOTE_SPEAKER`) | [wespeaker_en_voxceleb_resnet34_LM.onnx](https://github.com/k2-fsa/sherpa-onnx/releases/tag/speaker-recongition-models) (sherpa-onnx export), the same checkpoint as [pyannote/wespeaker-voxceleb-resnet34-LM](https://huggingface.co/pyannote/wespeaker-voxceleb-resnet34-LM) used by [pyannote/speaker-diarization-3.1](https://huggingface.co/pyannote/speaker-diarization-3.1) | CC BY 4.0 |
| Parakeet TDT 0.6B v3 (ONNX) | Speech-to-text | [istupakov/parakeet-tdt-0.6b-v3-onnx](https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx), converted from [nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) | CC BY 4.0 |
| Moonshine Streaming (Tiny/Small/Medium, English) | Speech-to-text | [moonshine-ai/moonshine-streaming](https://huggingface.co/moonshine-ai/moonshine-streaming) (formerly `UsefulSensors/moonshine-streaming`), `tokenizer.bin` from download.moonshine.ai | MIT |
| Whisper (ggml conversions) | Speech-to-text | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) | MIT |
| Whistle (`.cact`) | Speech-to-text (`VASDK_BUILD_WHISTLE_STT`) | [Cactus-Compute/whistle](https://huggingface.co/Cactus-Compute/whistle) | Apache 2.0 |
| Voz (downloaded by the Desert Ant Labs SDK at runtime) | Speech-to-text (`VASDK_BUILD_VOZ_STT`, Android app) | [desert-ant-labs/voz](https://huggingface.co/desert-ant-labs/voz), from [nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3); SDK [ai.desertant:voz](https://central.sonatype.com/artifact/ai.desertant/voz) | [Desert Ant Labs Source-Available License 1.0](https://license.desertant.com/1.0): free below 100k monthly active devices per platform, visible attribution required |
| Pocket TTS (ONNX) | Text-to-speech | [KevinAHM/pocket-tts-web](https://huggingface.co/spaces/KevinAHM/pocket-tts-web), converted from [kyutai/pocket-tts](https://huggingface.co/kyutai/pocket-tts) | CC BY 4.0 |
| Supertonic 3 | Text-to-speech | [supertone-oss-archive/supertonic-3](https://huggingface.co/supertone-oss-archive/supertonic-3) (archived, pinned revision) | OpenRAIL |
| Phonon 40m multilingual (GGUF) | Text-to-speech (`VASDK_BUILD_PHONON_TTS`, desktop default) | [gradium/phonon-40m-multilingual](https://huggingface.co/gradium/phonon-40m-multilingual) (gated) | CC BY-NC 4.0 |
| Phonon 31m English (GGUF) | Text-to-speech (`-DPHONON_TTS_MODEL=phonon_tts_en`, Android app) | [gradium/phonon-31m-en](https://huggingface.co/gradium/phonon-31m-en) | Apache 2.0 |
| Phonon voices (`.safetensors`) | Text-to-speech (Phonon) | [gradium/phonon-voices](https://huggingface.co/gradium/phonon-voices) | Apache 2.0 |
| MOSS-TTS-Nano 100M (ONNX) + MOSS-Audio-Tokenizer-Nano decoder | Text-to-speech (opt-in, `VASDK_BUILD_MOSS_TTS`) | [Supbatomic/moss-tts-nano-100m-onnx-model](https://huggingface.co/Supbatomic/moss-tts-nano-100m-onnx-model), exported from [OpenMOSS-Team/MOSS-TTS-Nano](https://huggingface.co/OpenMOSS-Team/MOSS-TTS-Nano); codec from [OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX](https://huggingface.co/OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX) | Apache 2.0 |
| LFM2.5 1.2B Instruct (GGUF) | Default local LLM (desktop apps) | [LiquidAI/LFM2.5-1.2B-Instruct-GGUF](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF) | LFM Open License v1.0 |
| LFM2.5 230M (GGUF) | Default local LLM (Android app) | [LiquidAI/LFM2.5-230M-GGUF](https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF) | LFM Open License v1.0 |
| RNNoise weights | Noise suppression | `rnnoise_data-<hash>.tar.gz` from media.xiph.org, the release matching the fetched [xiph/rnnoise](https://github.com/xiph/rnnoise) source | BSD 3-Clause |
| all-MiniLM-L6-v2 (ONNX) | Intent understanding: agents match paraphrases their examples miss (`VASDK_BUILD_MINILM_NLU`, on by default) | [sentence-transformers/all-MiniLM-L6-v2](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2) | Apache 2.0 |
| GLiNER small v2.1 / multi v2.1 (ONNX, int8) | Entity finding (opt-in, `VASDK_BUILD_GLINER_NER`; `VASDK_GLINER_MODEL=multi` for the multilingual one) | [onnx-community/gliner_small-v2.1](https://huggingface.co/onnx-community/gliner_small-v2.1), [onnx-community/gliner_multi-v2.1](https://huggingface.co/onnx-community/gliner_multi-v2.1), exported from [urchade/gliner_small-v2.1](https://huggingface.co/urchade/gliner_small-v2.1) and [urchade/gliner_multi-v2.1](https://huggingface.co/urchade/gliner_multi-v2.1) | Apache 2.0 |
| Llama 3.2 1B Instruct (GGUF) | Optional local LLM (Android app preset) | [bartowski/Llama-3.2-1B-Instruct-GGUF](https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF), quantized from [meta-llama/Llama-3.2-1B-Instruct](https://huggingface.co/meta-llama/Llama-3.2-1B-Instruct) | Llama 3.2 Community License |
| Qwen2.5 1.5B Instruct (GGUF) | Optional local LLM (Android app preset) | [bartowski/Qwen2.5-1.5B-Instruct-GGUF](https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF), quantized from [Qwen/Qwen2.5-1.5B-Instruct](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct) | Apache 2.0 |
| Gemma 3 1B IT (GGUF) | Optional local LLM (Android app preset) | [bartowski/google_gemma-3-1b-it-GGUF](https://huggingface.co/bartowski/google_gemma-3-1b-it-GGUF), quantized from [google/gemma-3-1b-it](https://huggingface.co/google/gemma-3-1b-it) | Gemma Terms of Use |

Points worth knowing before you ship a product on top of these:

- **CC BY 4.0** (Parakeet, Pocket TTS, WeSpeaker ResNet34-LM) requires attribution to the original model authors if you redistribute the weights or derivatives of them. WeSpeaker's VoxCeleb models carry the license of their training data, the [VoxCeleb dataset](https://mm.kaist.ac.kr/datasets/voxceleb/).
- **CC BY-NC 4.0** (the Phonon 40m model) forbids commercial use. Phonon is on by default on desktop and downloads the 40m model when a HuggingFace token is set. For a commercial product, license the weights from Gradium or switch to the Apache 2.0 [gradium/phonon-31m-en](https://huggingface.co/gradium/phonon-31m-en) with `-DPHONON_TTS_MODEL=phonon_tts_en` (the Android app already uses it). The voices are Apache 2.0 either way.
- **OpenRAIL** (Supertonic 3) is a responsible-AI license with use restrictions. Read the [license text on the model page](https://huggingface.co/supertone-oss-archive/supertonic-3) before commercial use.
- **LFM Open License v1.0** (both LFM2.5 GGUF models - the 1.2B desktop default and the 230M Android default) is Liquid AI's custom license with its own commercial terms. Read the LICENSE file in the respective model repository ([1.2B](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/blob/main/LICENSE), [230M](https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF)) before commercial use, or swap in a different GGUF via `-DVASDK_LLAMACPP_MODEL_URL` (desktop) or a different `LLMModelMeta` preset (Android).
- **Llama 3.2 Community License** (an Android preset) allows commercial use, with conditions: products with more than 700 million monthly active users need a separate license from Meta, use must follow Meta's Acceptable Use Policy, and a product that ships the model must include a copy of the license and display "Built with Llama". Read the [license](https://huggingface.co/meta-llama/Llama-3.2-1B-Instruct/blob/main/LICENSE.txt) before shipping it.
- **Gemma Terms of Use** (an Android preset) allow commercial use, subject to Google's Prohibited Use Policy, and pass the same restrictions on to anyone you redistribute the model to. Read the [terms](https://ai.google.dev/gemma/terms) before shipping it.
- **Moonshine:** all streaming models, which are the only Moonshine models VASDK downloads, are MIT. The non-commercial Moonshine Community License covers only the legacy non-streaming models for languages other than English.

## Evaluation data

The provider evaluation ([docs/evaluation.md](docs/evaluation.md)) runs on clips cut, resampled and mixed from two speech corpora. The set is downloaded at configure time with `VASDK_DOWNLOAD_EVAL_DATA=ON` (model `eval_audio_v1` of `VASDK_3P_IMPL/Evaluation/data/manifest.json`); nothing of it is in this repository. Its `ATTRIBUTION.md` travels with it.

| Corpus | Used for | Source | License |
| --- | --- | --- | --- |
| LibriSpeech ASR corpus, test-clean (Panayotov, Chen, Povey, Khudanpur) | English speech with speakers and genders; VAD clips; babble | [openslr.org/12](https://www.openslr.org/12) | CC BY 4.0 |
| FLEURS, en_us and fr_fr test (Conneau et al., Google) | English and French sentences, punctuated; turn clips; TTS sentences | [google/fleurs](https://huggingface.co/datasets/google/fleurs) | CC BY 4.0 |

The white, pink and brown noise and the hum are generated by the builder ([`tools/eval-data/build_eval_set.py`](tools/eval-data/build_eval_set.py)). Redistributing the set, or clips of it, needs the attribution above.

## Provider and library dependencies

Every VAD/STT/TTS/LLM/audio provider under `VASDK_3P_IMPL/`, plus the core library's own build dependencies, fetched from their upstream repository (or system-installed) at build time. Version/tag is whatever's currently pinned in the relevant `CMakeLists.txt` or `VASDK_3P_IMPL/cmake/*.cmake` script - check there for the exact commit if you need it. Six providers (`gladiapp`, `SonioxPP`, `deepgrampp`, `CartesiaPP`, `fishaudiopp`, `GradiumPP`) are VASDK's own thin API clients, not an SDK published by Gladia, Soniox, Deepgram, Cartesia, Fish Audio, or Gradium themselves; the license listed for those is for the wrapper code, not the underlying cloud service.

| Dependency | Used by | Source | License |
| --- | --- | --- | --- |
| [snakers4/silero-vad](https://github.com/snakers4/silero-vad) | VAD (Silero) | GitHub | MIT |
| [marton78/pffft](https://github.com/marton78/pffft) | SmartTurn v3, VizAnim, pyannote speaker embedding (FFT) | GitHub | Permissive NCAR/UCAR license |
| [xiph/rnnoise](https://github.com/xiph/rnnoise) | Noise suppression | GitHub | BSD 3-Clause |
| [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) | Silero, Parakeet, Moonshine, SmartTurn, PocketTTS, Supertonic, MOSS-TTS-Nano, MiniLM, GLiNER, pyannote speaker embedding (ONNX inference) | GitHub release archive (desktop), Maven Central AAR (Android) | MIT |
| [moonshine-ai/moonshine](https://github.com/moonshine-ai/moonshine) | STT (Moonshine streaming model, ORT utils and tokenizer code) | GitHub | MIT (code and every streaming model; only legacy non-streaming non-English models, which VASDK doesn't use, are under the non-commercial Moonshine Community License) |
| [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp) | STT (Whisper.cpp) | GitHub | MIT |
| Cactus needle engine | STT (Whistle; prebuilt binaries only, no source) | [Cactus-Compute/needle3](https://huggingface.co/Cactus-Compute/needle3) (HuggingFace: `cactus-needle` wheel on desktop, `libneedle.a` on Android) | Apache 2.0 |
| [fatehmtd/gladiapp](https://github.com/fatehmtd/gladiapp) | STT (Gladia, cloud) | GitHub | MIT |
| [fatehmtd/SonioxPP](https://github.com/fatehmtd/SonioxPP) | STT/TTS (Soniox, cloud) | GitHub | Apache 2.0 |
| [supertone-inc/supertonic](https://github.com/supertone-inc/supertonic) | TTS (Supertonic inference code) | GitHub | MIT |
| [google/sentencepiece](https://github.com/google/sentencepiece) | TTS (PocketTTS, Phonon and MOSS-TTS-Nano tokenizer), GLiNER's tokenizer | GitHub | Apache 2.0 |
| [ggml-org/ggml](https://github.com/ggml-org/ggml) | TTS (Phonon tensor ops, in-repo port of [gradium-ai/xn-ptts](https://github.com/gradium-ai/xn-ptts) (MIT OR Apache 2.0); shared with llama.cpp's copy when that is built) | GitHub | MIT |
| [fatehmtd/deepgrampp](https://github.com/fatehmtd/deepgrampp) | STT/TTS (Deepgram, cloud) | GitHub | Apache 2.0 |
| [fatehmtd/CartesiaPP](https://github.com/fatehmtd/CartesiaPP) | STT/TTS (Cartesia, cloud) | GitHub | Apache 2.0 |
| [fatehmtd/fishaudiopp](https://github.com/fatehmtd/fishaudiopp) | STT/TTS (Fish Audio, cloud) | GitHub | Apache 2.0 |
| [fatehmtd/GradiumPP](https://github.com/fatehmtd/GradiumPP) | STT/TTS (Gradium, cloud) | GitHub | Apache 2.0 |
| [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) | LLM (local GGUF inference) | GitHub | MIT |
| [mackron/miniaudio](https://github.com/mackron/miniaudio) | Audio I/O | GitHub (single header) | Public domain (Unlicense) / MIT-0, your choice |
| [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | Audio I/O (SDL3) | GitHub | zlib |
| FFmpeg | File/batch audio | System-installed (`find_package`), [ffmpeg.org](https://ffmpeg.org) | LGPL or GPL, depending on how your system build was configured |
| [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel) | WebRTC provider (`VASDK_BUILD_LIBDATACHANNEL_WEBRTC`) | GitHub | MPL 2.0 |
| [paullouisageneau/libjuice](https://github.com/paullouisageneau/libjuice), [cisco/libsrtp](https://github.com/cisco/libsrtp), [sctplab/usrsctp](https://github.com/sctplab/usrsctp), [SergiusTheBest/plog](https://github.com/SergiusTheBest/plog) | WebRTC provider (ICE, SRTP, SCTP, logging - libdatachannel submodules) | GitHub | MPL 2.0 (libjuice), BSD 3-Clause (libsrtp, usrsctp), MIT (plog) |
| [xiph/opus](https://github.com/xiph/opus) | WebRTC provider (Opus codec) | GitHub | BSD 3-Clause |
| OpenSSL | WebRTC provider (DTLS; Mbed TLS or GnuTLS via `USE_MBEDTLS`/`USE_GNUTLS` instead), libcurl built from source on Linux/macOS, cpp-httplib's HTTPS (the remote control's `tls`) on desktop. On Windows its DLLs ship beside `vasdk_c.dll` | System-installed (`find_package`), [openssl.org](https://www.openssl.org) | Apache 2.0 (3.x) |
| [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | TLS for libcurl and libwebsockets in the Android build | GitHub | Apache 2.0 OR GPL-2.0-or-later, your choice |
| [gabime/spdlog](https://github.com/gabime/spdlog) | Logging (core library) | GitHub | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON (core library) | GitHub | MIT |
| [CLIUtils/CLI11](https://github.com/CLIUtils/CLI11) | CLI parsing (showcase apps) | GitHub | BSD 3-Clause |
| [google/highway](https://github.com/google/highway) | SIMD (core library) | GitHub | Apache 2.0 / BSD 3-Clause, dual |
| [Compaile/ctrack](https://github.com/Compaile/ctrack) | Profiling (core library) | GitHub | MIT |
| [laserpants/dotenv-cpp](https://github.com/laserpants/dotenv-cpp) | `.env` file loading | GitHub | BSD 3-Clause |
| [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | WebSocket transport | GitHub | MIT |
| [ArthurSonzogni/FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal UI (`-DVASDK_BUILD_TUI=ON`) | GitHub | MIT |
| [curl/curl](https://github.com/curl/curl) (libcurl) | HTTP (OpenAI/Anthropic/Gladia/Deepgram/Cartesia/Fish Audio/Gradium transport) | An installed libcurl when CMake finds one, otherwise built from source (curl 8.21; always on Android). GradiumPP fetches its own copy unless another client already has | curl license (MIT-style) |
| [protocolbuffers/protobuf](https://github.com/protocolbuffers/protobuf) | Engine config files, events and steps as protobuf messages (core library, Python binding) | An installed protobuf when CMake finds one, otherwise built from source; prebuilt `protoc` release binary for cross builds | BSD 3-Clause |
| [abseil/abseil-cpp](https://github.com/abseil/abseil-cpp) | protobuf, when protobuf is built from source | GitHub | Apache 2.0 |
| [java-native-access/jna](https://github.com/java-native-access/jna) | The Kotlin binding's calls into the C API (`VASDK_BUILD_KOTLIN_BINDING`, the Android AAR) | Maven Central | Apache 2.0 or LGPL 2.1, the user's choice; VASDK takes it under Apache 2.0 |
| [protocolbuffers/protobuf](https://github.com/protocolbuffers/protobuf) (protobuf-kotlin-lite, protobuf-javalite) | The Kotlin binding's messages | Maven Central | BSD 3-Clause |
| [Kotlin/kotlinx.coroutines](https://github.com/Kotlin/kotlinx.coroutines) | The Kotlin binding's event flow | Maven Central | Apache 2.0 |
| [Kotlin/kotlinx.serialization](https://github.com/Kotlin/kotlinx.serialization) (kotlinx-serialization-json) | The Kotlin binding's JSON (tools, entity models, LLM requests) | Maven Central | Apache 2.0 |
| [warmcat/libwebsockets](https://github.com/warmcat/libwebsockets) | WebSocket client in CartesiaPP, fishaudiopp and GradiumPP | GitHub (fetched by those clients) | MIT (a few files BSD 3-Clause, zlib or, with Mbed TLS, Apache 2.0) |
| [tplgy/cppcodec](https://github.com/tplgy/cppcodec) | Base64 in SonioxPP and GradiumPP | GitHub | MIT |
| [tobiaslocker/base64](https://github.com/tobiaslocker/base64) | Base64 in gladiapp | GitHub (fetched by gladiapp) | MIT |
| OpenAI API | LLM (cloud) | [openai.com](https://openai.com) / [API docs](https://platform.openai.com/docs) - direct HTTP, no vendor SDK | Governed by OpenAI's API terms, not an OSS license |
| Anthropic API | LLM (cloud) | [anthropic.com](https://www.anthropic.com) / [API docs](https://platform.claude.com/docs) - direct HTTP, no vendor SDK | Governed by Anthropic's API terms, not an OSS license |

### Python packages

Installed from PyPI by uv or pip (`bindings/python/pyproject.toml`, `VASDK_PYTHON_APP/pyproject.toml` and `uv.lock`, `VASDK_PYTHON_APP/requirements.txt`), not bundled in a wheel.

| Package | Used by | License |
| --- | --- | --- |
| [protobuf](https://pypi.org/project/protobuf/) | The Python binding (`vasdk`): VASDK's messages | BSD 3-Clause |
| [sounddevice](https://pypi.org/project/sounddevice/) (with [PortAudio](https://www.portaudio.com/), bundled in its macOS and Windows wheels) | The Python app's voice mode: microphone and speaker | MIT (PortAudio: MIT-style) |
| [torch](https://pypi.org/project/torch/), [torchaudio](https://pypi.org/project/torchaudio/) | Development only: `gen_fbank_golden.py` writes the pyannote fbank golden | BSD 3-Clause / BSD 2-Clause |
| [transformers](https://pypi.org/project/transformers/), [numpy](https://pypi.org/project/numpy/) | Development only: `gen_features_golden.py` writes the SmartTurn features golden | Apache 2.0 / BSD 3-Clause |

### Android app libraries

Pulled from Google Maven and Maven Central by Gradle and packaged into the APK (see `VASDK_ANDROID/app/build.gradle`).

| Dependency | Used for | License |
| --- | --- | --- |
| AndroidX (core-ktx, appcompat, activity-compose, lifecycle, constraintlayout, recyclerview) and Jetpack Compose (ui, foundation, material3, material-icons-extended) | App UI | Apache 2.0 |
| Material Components for Android | App UI | Apache 2.0 |
| `com.google.protobuf:protobuf-kotlin-lite` | Events and steps as protobuf messages | BSD 3-Clause |
| ML Kit (`vision-common`, `barcode-scanning-common`) | Declared in `build.gradle`; no app code uses it yet | [ML Kit Terms of Service](https://developers.google.com/ml-kit/terms), not an OSS license |
| `ai.desertant:voz` | Voz STT | [Desert Ant Labs Source-Available License 1.0](https://license.desertant.com/1.0) |

## Models bundled in this repository

The Android app embeds two model files at `VASDK_ANDROID/app/src/main/res/raw/`. Their full license texts follow, as their terms require.

### Silero VAD (`silero_vad.onnx`)

Source: https://github.com/snakers4/silero-vad

```text
MIT License

Copyright (c) 2020-present Silero Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Smart Turn v3 (`smart_turn_v3.onnx`)

Source: https://huggingface.co/pipecat-ai/smart-turn-v3

```text
BSD 2-Clause License

Copyright (c) 2024–2025, Daily

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
