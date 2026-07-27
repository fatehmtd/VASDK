# Third-Party Notices

VASDK itself is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)). The models it runs are not. This file lists every model the project uses, where it comes from, and the license it is distributed under.

Two model files are bundled directly in this repository (for the Android app). Everything else is downloaded from its upstream source at CMake configure time by the scripts in `VASDK_3P_IMPL/cmake/`; those files are never redistributed here, but their licenses still govern how you may use and ship them.

Library and provider dependencies (spdlog, nlohmann/json, llama.cpp, whisper.cpp, ONNX Runtime, CLI11, and the rest) are fetched from their upstream repositories at build time under their own licenses and are not redistributed in this repository. They're itemized in [Provider and library dependencies](#provider-and-library-dependencies) below.

## Model summary

| Model | Pipeline stage | Source | License |
| --- | --- | --- | --- |
| Silero VAD | Voice activity detection | [snakers4/silero-vad](https://github.com/snakers4/silero-vad) | MIT |
| Smart Turn v3 | Turn detection | [pipecat-ai/smart-turn-v3](https://huggingface.co/pipecat-ai/smart-turn-v3) | BSD 2-Clause |
| Parakeet TDT 0.6B v3 (ONNX) | Speech-to-text | [istupakov/parakeet-tdt-0.6b-v3-onnx](https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx), converted from [nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) | CC BY 4.0 |
| Moonshine Base | Speech-to-text | [UsefulSensors/moonshine](https://huggingface.co/UsefulSensors/moonshine) | MIT |
| Whisper (ggml conversions) | Speech-to-text | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) | MIT |
| Pocket TTS (ONNX) | Text-to-speech | [KevinAHM/pocket-tts-web](https://huggingface.co/spaces/KevinAHM/pocket-tts-web), converted from [kyutai/pocket-tts](https://huggingface.co/kyutai/pocket-tts) | CC BY 4.0 |
| Supertonic 2 | Text-to-speech | [Supertone/supertonic-2](https://huggingface.co/Supertone/supertonic-2) | OpenRAIL |
| LFM2.5 1.2B Instruct (GGUF) | Default local LLM (desktop apps) | [LiquidAI/LFM2.5-1.2B-Instruct-GGUF](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF) | LFM Open License v1.0 |
| LFM2.5 230M (GGUF) | Default local LLM (Android app) | [LiquidAI/LFM2.5-230M-GGUF](https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF) | LFM Open License v1.0 |
| RNNoise weights | Noise suppression | [xiph/rnnoise](https://github.com/xiph/rnnoise) (weights ship with the fetched source) | BSD 3-Clause |

Points worth knowing before you ship a product on top of these:

- **CC BY 4.0** (Parakeet, Pocket TTS) requires attribution to the original model authors if you redistribute the weights or derivatives of them.
- **OpenRAIL** (Supertonic 2) is a responsible-AI license with use restrictions. Read the [license text on the model page](https://huggingface.co/Supertone/supertonic-2) before commercial use.
- **LFM Open License v1.0** (both LFM2.5 GGUF models - the 1.2B desktop default and the 230M Android default) is Liquid AI's custom license with its own commercial terms. Read the LICENSE file in the respective model repository ([1.2B](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/blob/main/LICENSE), [230M](https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF)) before commercial use, or swap in a different GGUF via `-DVASDK_LLAMACPP_MODEL_URL` (desktop) or a different `LLMModelMeta` preset (Android).

## Provider and library dependencies

Every VAD/STT/TTS/LLM/audio provider under `VASDK_3P_IMPL/`, plus the core library's own build dependencies, fetched from their upstream repository (or system-installed) at build time. Version/tag is whatever's currently pinned in the relevant `CMakeLists.txt` or `VASDK_3P_IMPL/cmake/*.cmake` script - check there for the exact commit if you need it. Three providers (`gladiapp`, `SonioxPP`, `deepgrampp`) are VASDK's own thin API clients, not an SDK published by Gladia, Soniox, or Deepgram themselves; the license listed for those is for the wrapper code, not the underlying cloud service.

| Dependency | Used by | Source | License |
| --- | --- | --- | --- |
| [snakers4/silero-vad](https://github.com/snakers4/silero-vad) | VAD (Silero) | GitHub | MIT |
| [marton78/pffft](https://github.com/marton78/pffft) | SmartTurn v3, VizAnim (FFT) | GitHub | Permissive NCAR/UCAR license |
| [xiph/rnnoise](https://github.com/xiph/rnnoise) | Noise suppression | GitHub | BSD 3-Clause |
| [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) | Silero, Parakeet, SmartTurn, PocketTTS, Supertonic (ONNX inference) | GitHub (release archive) | MIT |
| [moonshine-ai/moonshine](https://github.com/moonshine-ai/moonshine) | STT (Moonshine tokenizer/runtime code) | GitHub | MIT (English models); non-commercial Moonshine Community License for other languages |
| [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp) | STT (Whisper.cpp) | GitHub | MIT |
| [fatehmtd/gladiapp](https://github.com/fatehmtd/gladiapp) | STT (Gladia, cloud) | GitHub | MIT |
| [fatehmtd/SonioxPP](https://github.com/fatehmtd/SonioxPP) | STT/TTS (Soniox, cloud) | GitHub | Apache 2.0 |
| [supertone-inc/supertonic](https://github.com/supertone-inc/supertonic) | TTS (Supertonic inference code) | GitHub | MIT |
| [google/sentencepiece](https://github.com/google/sentencepiece) | TTS (PocketTTS tokenizer) | GitHub | Apache 2.0 |
| [fatehmtd/deepgrampp](https://github.com/fatehmtd/deepgrampp) | STT/TTS (Deepgram, cloud) | GitHub | Apache 2.0 |
| [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) | LLM (local GGUF inference) | GitHub | MIT |
| [mackron/miniaudio](https://github.com/mackron/miniaudio) | Audio I/O | GitHub (single header) | Public domain (Unlicense) / MIT-0, your choice |
| [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | Audio I/O (SDL3) | GitHub | zlib |
| FFmpeg | File/batch audio | System-installed (`find_package`), [ffmpeg.org](https://ffmpeg.org) | LGPL or GPL, depending on how your system build was configured |
| [gabime/spdlog](https://github.com/gabime/spdlog) | Logging (core library) | GitHub | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON (core library) | GitHub | MIT |
| [CLIUtils/CLI11](https://github.com/CLIUtils/CLI11) | CLI parsing (showcase apps) | GitHub | BSD 3-Clause |
| [google/highway](https://github.com/google/highway) | SIMD (core library) | GitHub | Apache 2.0 / BSD 3-Clause, dual |
| [Compaile/ctrack](https://github.com/Compaile/ctrack) | Profiling (core library) | GitHub | MIT |
| [laserpants/dotenv-cpp](https://github.com/laserpants/dotenv-cpp) | `.env` file loading | GitHub | BSD 3-Clause |
| [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | WebSocket transport | GitHub | MIT |
| [ArthurSonzogni/FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal UI (`-DVASDK_BUILD_TUI=ON`) | GitHub | MIT |
| libcurl | HTTP (OpenAI/Anthropic/Gladia/Deepgram transport) | System-installed (`find_package(CURL)`) | MIT-style (curl license) |
| OpenAI API | LLM (cloud) | [openai.com](https://openai.com) / [API docs](https://platform.openai.com/docs) - direct HTTP, no vendor SDK | Governed by OpenAI's API terms, not an OSS license |
| Anthropic API | LLM (cloud) | [anthropic.com](https://www.anthropic.com) / [API docs](https://platform.claude.com/docs) - direct HTTP, no vendor SDK | Governed by Anthropic's API terms, not an OSS license |

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
