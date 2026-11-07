---
name: vasdk-audio-io
description: Audio in and out of VASDK - microphone sources and speaker sinks (MiniAudio, SDL3, FFmpeg files, Android AAudio), audio formats and the ring buffer, playback hold and fades for barge-in, and in-line DSP such as RNNoise noise suppression. Use when working on capture, playback, clicks, dropped frames, echo, or adding an audio backend or DSP.
---

# Audio I/O and DSP

**What it does:** gets samples from a device or file into the pipeline and the assistant's voice back out. `LocalTransport` runs a capture thread over a `GenericAudioSource` and plays through a `GenericAudioSink`; `AudioDSPProcessor` runs a `GenericAudioDSPProvider` (noise suppression) on the mic audio before VAD and STT.

| | |
| --- | --- |
| Source / sink | `VASDK/include/VASDK/GenericAudioSource.h`, `VASDK/include/VASDK/GenericAudioSink.h`, [contracts](../../docs/reference/providers.md#audio-io) |
| DSP | `VASDK/include/VASDK/GenericAudioDSPProvider.h`, `VASDK/include/VASDK/AudioDSPProcessor.h`, [contract](../../docs/reference/providers.md#genericaudiodspprovider) |
| Formats | `AudioConfig` in `VASDK/include/VASDK/CommonAudioUtils.h`; [the ring buffer](../../docs/reference/frames.md#the-audio-ring-buffer) |
| Transport | `VASDK/include/VASDK/LocalTransport.h`, [notes](../../docs/reference/transports.md#localtransport) |
| Config | `audio { input output }`, [engine config](../../docs/reference/engine-config.md#audio); the device itself: `providers { audio_input { id: "miniaudio" } audio_output { id: "miniaudio" } }`, so a session (and the C API's) opens the mic and speaker |

## Built in

| Backend | Folder | Kind |
| --- | --- | --- |
| MiniAudio | `VASDK_3P_IMPL/MiniAudio/`; config id `miniaudio` | mic + speaker, macOS/Linux/Windows, on by default |
| SDL3 | `VASDK_3P_IMPL/SDL/` | mic + speaker |
| FFmpeg | `VASDK_3P_IMPL/FFmpegAudio/` (`-DVASDK_BUILD_FFMPEG_AUDIO=ON`) | file source and a file transport for batch audio; turned off with a warning when FFmpeg isn't found, and on Android |
| AAudio | `VASDK_3P_IMPL/AAudio/` (`-DVASDK_BUILD_AAUDIO=ON`, on for Android); config id `aaudio` | Android mic + speaker; off with a warning elsewhere |
| RNNoise | `VASDK_3P_IMPL/RNNoise/` | noise suppression DSP, on by default; off with a warning on Android |

These are not in the provider catalog: an app constructs them and hands them to `LocalTransport` (see `VASDK_APP/main_pipeline.cpp`).

## Contract pitfalls

- Offsets and lengths are in **samples**; buffers are raw bytes in the configured format.
- Source: override `waitForData(timeoutMs)` with a condition variable; the polling default costs latency and drops frames under load.
- Sink: `getPendingAudioBytes()` tells drain logic when playback really ended. `flushAudio()` / `fadeAndFlush()` (no click) are how barge-in stops the voice. `holdPlayback(mode, held, duckGainDecibels)` ducks or pauses during a possible back-channel and must resume from the same sample; pull-model sinks reuse `VASDK/include/VASDK/PlaybackHoldEnvelope.h`.
- DSP: `processAudio()` writes exactly `numSamples` out, in the input format; anything else desynchronizes every downstream offset.
- The assistant's own voice from open speakers must not interrupt it: see the `turn_taking` settings for it in `VASDK/proto/vasdk/engine/engine_config.proto`.

## Adding one

1. Code rules from [vasdk-provider](../vasdk-provider/SKILL.md#code-rules): `va::thirdparty::audio`, `<Name>Options` with defaults (device name, buffer size, latency), no statics, every platform or warn and document. Files: rows 1, 2, 9, 11-16 of its list; there is no catalog row until audio backends become selectable in the engine config.
2. Templates: `VASDK_3P_IMPL/MiniAudio/` (source and sink; `MiniAudioSourceOptions`) and `VASDK_3P_IMPL/RNNoise/` (DSP; `RNNoiseDSPOptions`).
3. A backend for one platform only (as AAudio is for Android) states so on its page and in the README, with the CMake warning elsewhere.

## Tests

- Unit: in `VASDK_3P_IMPL/<Name>/tests/`. For a sink, feed a known signal through a null or loopback device (skip with 77 when there is no audio device, as on CI), and check `flushAudio()`, the resume position after `holdPlayback()`, and `getPendingAudioBytes()` reaching 0. For DSP, check the sample count is kept and a noisy tone comes out with less noise.
- Integration: a pipeline with `MockTransport` and `AudioDSPProcessor` for DSP ([testing without a microphone](../../docs/examples/testing-with-mock-transport.md)); `vasdk_backchannel` for hold behaviour; a run of `VASDK_Pipeline` with the backend for a device backend.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds, and its page lists the platforms and the latency measured.
