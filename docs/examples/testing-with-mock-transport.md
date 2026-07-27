# Example: testing without a microphone

`MockTransport` is a transport that does no real I/O. You inject frames into it as if they arrived from the outside world, run the pipeline, and inspect what the pipeline tried to send out. That makes full pipeline behavior testable in unit tests and CI, with no audio hardware and no network.

## Driving a pipeline with injected frames

```cpp
#include <VASDK/MockTransport.h>

auto mock = std::make_shared<va::transport::MockTransport>();

// Simulate "receiving" a final transcription, as if STT had just produced it
auto frame = std::make_shared<va::transport::TranscriptionFrame>();
frame->transcriptionResult.text = "book me a callback for tomorrow morning";
frame->isFinal = true;
mock->injectFrame(frame);

// ... run the pipeline ...

// Inspect what the pipeline sent out
auto synthesized = mock->getSentFramesByType(va::transport::FrameType::SYNTHESIS);
auto everything  = mock->getSentFrames();
mock->clearSentFrames();
```

`injectFrame()` simulates the receive side; `sendFrame()` calls made by the pipeline are captured instead of transmitted, and `getSentFrames()` / `getSentFramesByType()` let you assert on them. `clearSentFrames()` resets between test cases.

You can inject any frame type, which means you can start a test at any stage of the pipeline: inject `AudioFrame`s to exercise VAD onward, `TranscriptionFrame`s to skip straight to the LLM, or `ControlFrame`s to test interrupt handling.

## Scripting whole conversations

For goal-oriented pipelines there's a purpose-built harness on top of this idea: `GoalConversationSimulator` (in `VASDK/include/VASDK/goal/`) scripts a fake caller through a goal, turn by turn, and asserts on the resulting `GoalState`. Paired with a scripted LLM stand-in, it regression-tests slot filling, confirmation, and escalation logic without a live model, deterministically.

## Full reference

See [`MockTransport.h`](../../VASDK/include/VASDK/MockTransport.h) and [`GoalConversationSimulator.h`](../../VASDK/include/VASDK/goal/GoalConversationSimulator.h) for the complete APIs.
