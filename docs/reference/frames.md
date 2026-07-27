# Frames

*Reference for `va::transport::Frame` and every concrete frame type. Header: [`Frame.h`](../../VASDK/include/VASDK/Frame.h) - the source of truth if this page and the code ever disagree.*

Frames are the only thing that moves through a VASDK pipeline. Every stage communicates by producing and consuming frames; no processor calls another processor's methods directly. If you understand the frame taxonomy, you can read any pipeline.

## The base class

```cpp
class Frame {
    virtual FrameType getType() const = 0;                 // which kind of frame this is
    virtual FramePriority getPriority() const;             // queue ordering (see below)
    virtual std::shared_ptr<Frame> clone() const = 0;      // deep copy

    uint64_t    timestamp;            // creation time, microseconds since epoch
    uint64_t    sequenceId;           // ordering within a stream
    std::string frameId;              // unique ID for debugging
    std::string sourceId;             // which component created it
    uint64_t    processingDuration;   // perf monitoring
    uint64_t    acquisitionDuration;  // time spent waiting (e.g. for audio)
};
```

Two contracts every new frame type must honor:

1. **`getType()` returns a stable `FrameType`** - processors filter on it in `shouldProcess()`.
2. **`clone()` is a deep copy** - including all base-class metadata. Every existing frame type copies `timestamp`, `sequenceId`, `frameId`, and `sourceId` explicitly; follow that pattern.

## Priorities

Processors use a priority queue: higher-priority frames jump ahead of normal traffic, FIFO order is preserved within the same priority. Defaults:

| Frame | Priority |
| --- | --- |
| `SYSTEM`, `ERROR` types (base-class default) | `CRITICAL` |
| `ControlFrame` with `INTERRUPT` | `CRITICAL` |
| `ControlFrame` (any other command) | `HIGH` |
| `ErrorFrame` with `FATAL` severity | `CRITICAL` |
| `ErrorFrame` (other severities) | `HIGH` |
| `AudioSourceEndControlFrame` | `LOW` (end-of-file marker shouldn't overtake audio) |
| Everything else | `NORMAL` |

Override `getPriority()` in a custom frame only when it genuinely must preempt audio processing - an over-prioritized chatty frame type starves the media path.

## Flow directions

Frames travel `DOWNSTREAM` (input → output: audio, transcriptions, LLM text, synthesis) or `UPSTREAM` (output → input: errors, interrupts, TTS lifecycle events). The direction is a property of *how a frame is pushed*, not of the frame itself - the same `ControlFrame` type can go either way.

## Frame catalog

Every `FrameType`, its concrete class, and what carries it:

| `FrameType` | Class | Payload | Typical producer → consumer |
| --- | --- | --- | --- |
| `AUDIO` | `AudioFrame` | **No samples** - `absoluteOffset` + `numSamples` + format metadata referencing a shared [`AudioRingBuffer`](../../VASDK/include/VASDK/AudioRingBuffer.h) | `LocalTransport` → DSP, VAD |
| `VAD_STATE` | `VADStateFrame` | `VADState` + speech start/end offsets + confidence | `VADFrameProcessor` → turn detection, transcription |
| `TURN_STATE` | `TurnStateFrame` | actor (`USER`/`SYSTEM`/`OTHER`), state (`START`/`END`), type (`NORMAL`/`INTERRUPT`/`OVERLAP`), confidence | `TurnDetectionProcessor` → `AgentStateProcessor` |
| `TRANSCRIPTION` | `TranscriptionFrame` | full `TranscriptionResult` (text, confidence, word timings, language tags, alternatives) + `isFinal` | `TranscriptionFrameProcessor` → agent state, LLM |
| `AGENT_STATE` | `AgentStateFrame` | current/previous `AgentState`, the `AgentStateTransition` that caused it, trigger confidence | `AgentStateProcessor` → LLM, TTS |
| `TEXT` | `TextFrame` | text, language, `isFinal` (mid-stream vs last chunk), `isExpecting` (LLM awaits a spoken reply), UI `choices` | `LLMProcessor` → TTS, UI transports |
| `TOOL_CALL` | `ToolCallFrame` | tool name + JSON args (as a string, to keep nlohmann/json out of `Frame.h`) | `LLMProcessor` → `ToolDispatchProcessor` |
| `TOOL_RESULT` | `ToolResultFrame` | tool name + JSON result string | `ToolDispatchProcessor` → back to LLM |
| `SYNTHESIS` | `SynthesisFrame` | **actual audio bytes** (`audioData`) + source text + voice ID | `TTSFrameProcessor` → output transport |
| `SYNTHESIS_STATE` | `SynthesisStateFrame` | `STARTED` / `COMPLETED` / `INTERRUPTED` / `ERROR` - flows **upstream** | `TTSFrameProcessor` → agent state, turn detection |
| `TRIGGER` | `TriggerFrame` | `WAKE_WORD` / `BUTTON_PRESS` / `APP_COMMAND` / `SCHEDULED` + data + confidence | `TriggerProcessor` → `AgentStateProcessor` |
| `CONTROL` | `ControlFrame` | `START` / `STOP` / `PAUSE` / `RESUME` / `RESET` / `INTERRUPT` + optional `targetId` | any → any |
| `METADATA` | `MetadataFrame` | key/value string pair | app-specific |
| `ERROR` | `ErrorFrame` | severity (`WARNING`/`ERROR`/`FATAL`) + code + message - flows **upstream** | any failing stage → `RecoveryProcessor`, app |
| `SYSTEM` | *(reserved)* | high-priority control category | - |
| `AUDIO_FEATURES` | *(VizAnim provider)* | extracted feature vector for UI visualization | audio analysis → UI |

Note the asymmetry that trips people up: **`AudioFrame` (mic side) carries no audio; `SynthesisFrame` (TTS side) does.** Mic audio is a continuous firehose shared by several consumers, so it lives once in a ring buffer and frames carry offsets. Synthesized audio is bursty and single-consumer, so it rides in the frame. The [root README's ring buffer section](../../README.md#the-audio-ring-buffer) explains the design in depth.

## The two state machines encoded in frames

Two enums deserve special attention because processors key their behavior off them:

**`VADState`** (acoustic, emitted by `VADFrameProcessor`): `QUIET → STARTING → ONGOING → PAUSED ⇄ RESUMING → ENDING → ENDED`. Distinguishes a hesitation (`PAUSED`) from the end of an utterance (`ENDED`).

**`AgentState`** (conversational, emitted by `AgentStateProcessor`): `IDLE → LISTENING → THINKING → SPEAKING → INTERRUPTED`. See [processors.md](processors.md#agentstateprocessor--the-conversation-state-machine) for the full transition diagram.

## Adding a new frame type

The enum and catalog above will grow. To add a frame type without breaking existing pipelines:

1. Add the value to `FrameType` **and** to the `toString(FrameType)` switch in `Frame.h`.
2. Subclass `Frame`; implement `getType()`, a deep `clone()`, and (only if justified) `getPriority()`.
3. Keep heavy dependencies out of the header - serialize complex payloads to strings (see `ToolCallFrame::argsJson` for the precedent).
4. Existing processors ignore unknown types by default (`shouldProcess()` filters), so a new frame type is backward-compatible as long as you don't repurpose an existing one.
