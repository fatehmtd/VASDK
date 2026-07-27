# Example: a minimal pipeline

The smallest useful VASDK program chains six processors between an input and an output transport: audio in, VAD, STT, LLM, TTS, audio out. Processors are chained in the order you add them, and each runs on its own thread once the pipeline starts.

## The pipeline

```cpp
#include <VASDK/Pipeline.h>

auto pipeline = std::make_shared<va::pipeline::Pipeline>();
pipeline->addProcessor(transportInput)
    .addProcessor(vadProcessor)
    .addProcessor(transcription)
    .addProcessor(llmProcessor)
    .addProcessor(tts)
    .addProcessor(transportOutput);

pipeline->build();   // wires each processor's upstream/downstream links
pipeline->start();   // starts every processor's thread
```

`build()` connects neighbors; `start()` brings the whole chain up. Frames then flow downstream (audio to text to synthesis) while errors, interrupts, and state changes flow upstream, without the pipeline itself orchestrating any of it.

## Constructing the stages

Each processor wraps a provider. For a fully local stack:

```cpp
// Shared audio ring buffer: AudioFrames reference ranges in it instead of carrying samples
auto ringBuffer = std::make_shared<va::AudioRingBuffer>(/* capacity */);

// VAD: Silero ONNX model
auto vadProvider  = std::make_shared<SileroVAD>(modelsPath + "/silero_vad.onnx");
auto vadProcessor = std::make_shared<va::pipeline::VADFrameProcessor>(vadProvider);
vadProcessor->setRingBuffer(ringBuffer);

// STT: Parakeet ONNX model
auto sttProvider   = std::make_shared<ParakeetTranscriptionProvider>(modelsPath + "/parakeet");
auto transcription = std::make_shared<va::pipeline::TranscriptionFrameProcessor>(sttProvider);
transcription->setRingBuffer(ringBuffer);

// TTS: Supertonic ONNX model
auto ttsProvider = std::make_shared<SupertonicVoiceSynthesisProvider>(
    modelsPath + "/supertonic/onnx",
    modelsPath + "/supertonic/voice_styles/M1.json");
auto tts = std::make_shared<va::pipeline::TTSFrameProcessor>(ttsProvider);
```

Processors that read audio (VAD, STT) share the ring buffer via `setRingBuffer()`; the transport writes into it, and `AudioFrame`s only carry offsets.

Swapping any stage means constructing a different provider and handing it to the same processor type. The rest of the chain doesn't change.

## Linking

Link the core library plus one target per provider you use:

```cmake
target_link_libraries(my_app
    PRIVATE VASDK
    PRIVATE vasdk_miniaudio_audio
    PRIVATE vasdk_silero_vad
    PRIVATE vasdk_parakeet
    PRIVATE vasdk_supertonic_tts
)
```

## Full reference

[`VASDK_APP/main_pipeline.cpp`](../../VASDK_APP/main_pipeline.cpp) is the complete, working version of this example, including audio device setup, turn detection, agent state management, tool dispatch, and metrics. See the [VASDK_APP guide](../../VASDK_APP/README.md).
