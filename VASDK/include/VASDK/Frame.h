#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

#include "vasdk_export.h"
#include "GenericTranscriptionProvider.h"

namespace va {
    namespace transport {

        /**
         * @brief Frame types for the transport layer
         */
        enum class FrameType {
            SYSTEM,           // High-priority control frames (interruptions, errors)
            AUDIO,            // Raw audio data
            TEXT,             // Text data (transcription input/output)
            VAD_STATE,        // Voice Activity Detection state changes
            TURN_STATE,       // Dialog turn state (start, end)
            TRANSCRIPTION,    // Speech-to-Text results
            SYNTHESIS,        // Text-to-Speech results
            SYNTHESIS_STATE,  // TTS lifecycle events (started, completed, interrupted)
            AGENT_STATE,      // Agent conversation state machine events
            TRIGGER,          // Wake word or button trigger events
            CONTROL,          // Pipeline control (start, stop, pause, resume)
            METADATA,         // Additional metadata (user context, session info)
            ERROR,            // Error information
            AUDIO_FEATURES,   // Extracted audio feature vector (VizAnim)
            TOOL_CALL,        // LLM-requested tool invocation
            TOOL_RESULT       // Result returned by a tool handler
        };

        inline std::string toString(FrameType type) {
            switch (type) {
            case FrameType::SYSTEM:           return "SYSTEM";
            case FrameType::AUDIO:            return "AUDIO";
            case FrameType::TEXT:             return "TEXT";
            case FrameType::VAD_STATE:        return "VAD_STATE";
            case FrameType::TURN_STATE:       return "TURN_STATE";
            case FrameType::TRANSCRIPTION:    return "TRANSCRIPTION";
            case FrameType::SYNTHESIS:        return "SYNTHESIS";
            case FrameType::SYNTHESIS_STATE:  return "SYNTHESIS_STATE";
            case FrameType::AGENT_STATE:      return "AGENT_STATE";
            case FrameType::TRIGGER:          return "TRIGGER";
            case FrameType::CONTROL:          return "CONTROL";
            case FrameType::METADATA:         return "METADATA";
            case FrameType::ERROR:            return "ERROR";
            case FrameType::AUDIO_FEATURES:   return "AUDIO_FEATURES";
            case FrameType::TOOL_CALL:        return "TOOL_CALL";
            case FrameType::TOOL_RESULT:      return "TOOL_RESULT";
            }
            return "UNKNOWN";
        }

        /**
         * @brief Priority level for frame processing
         */
        enum class FramePriority {
            LOW = 0,
            NORMAL = 1,
            HIGH = 2,
            CRITICAL = 3  // System frames
        };

        /**
         * @brief Base class for all frames in the transport system
         *
         * Frames are the fundamental unit of data flow in VASDK.
         * They carry metadata and payload through the processing pipeline.
         */
        class VASDK_EXPORT Frame {
            public:
            virtual ~Frame() = default;

            /**
             * @brief Get the type of this frame
             * @return FrameType
             */
            virtual FrameType getType() const = 0;

            /**
             * @brief Get the priority of this frame
             * @return FramePriority
             */
            virtual FramePriority getPriority() const {
                // System frames get highest priority by default
                return (getType() == FrameType::SYSTEM || getType() == FrameType::ERROR)
                    ? FramePriority::CRITICAL
                    : FramePriority::NORMAL;
            }

            /**
             * @brief Clone this frame (deep copy)
             * @return Shared pointer to cloned frame
             */
            virtual std::shared_ptr<Frame> clone() const = 0;

            // Metadata
            uint64_t timestamp;         // Timestamp in microseconds
            uint64_t sequenceId;        // Sequence number for ordering
            std::string frameId;        // Unique identifier for debugging
            std::string sourceId;       // ID of the component that created this frame

            // duration of processing this frame (for performance monitoring)
            uint64_t processingDuration;

            // Acquisition duration for frames that involve waiting (e.g., waiting for audio data)
            uint64_t acquisitionDuration;

            /**
             * @brief Get current timestamp in microseconds
             * @return uint64_t
             */
            static uint64_t getCurrentTimestamp() {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
            }

            protected:
            Frame()
                : timestamp(getCurrentTimestamp())
                , sequenceId(0)
                , frameId("")
                , sourceId("")
                , processingDuration(0)
                , acquisitionDuration(0) {
            }
        };

        /**
         * @brief Audio frame - lightweight view into shared ring buffer
         *
         * This frame does not contain actual audio samples. Instead, it carries
         * metadata that describes a range of samples in a shared AudioRingBuffer.
         * Processors should read audio data from the ring buffer using absoluteOffset
         * and numSamples.
         */
        class VASDK_EXPORT AudioFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::AUDIO; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<AudioFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->numSamples = numSamples;
                cloned->sampleRate = sampleRate;
                cloned->channels = channels;
                cloned->format = format;
                cloned->absoluteOffset = absoluteOffset;
                cloned->sampleSizeInBytes = sampleSizeInBytes;
                cloned->ringbufferOffset = ringbufferOffset;
                cloned->postWriteOffset = postWriteOffset;
                return cloned;
            }

            // Audio metadata (no actual samples - read from ring buffer)
            uint32_t numSamples{ 0 };        // Number of samples this frame represents
            uint32_t sampleSizeInBytes{ 0 };  // Size of each sample in bytes
            uint32_t sampleRate{ 16000 };    // Sample rate in Hz (e.g., 16000)
            uint32_t channels{ 1 };          // Number of channels (1 = mono, 2 = stereo)
            int format;                   // Audio format (e.g., PCM16, PCM32, FLOAT)
            uint64_t absoluteOffset{ 0 };    // Absolute position in audio stream
            uint64_t ringbufferOffset{ 0 };    // Relative position in ring buffer 
            uint64_t postWriteOffset{ 0 };     // Absolute offset after writing to ring buffer (for debugging)

            /**
             * @brief Get the size of the audio data in bytes
             */
            size_t getSizeInBytes() const {
                return static_cast<size_t>(numSamples) * sampleSizeInBytes;
            }
        };

        /**
         * @brief Text frame for transcription and synthesis
         */
        class VASDK_EXPORT TextFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::TEXT; }

            /// A selectable choice presented to the user in a UI (populated by the LLM payload).
            struct Choice {
                std::string text;
            };

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<TextFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->text = text;
                cloned->language = language;
                cloned->isFinal = isFinal;
                cloned->isExpecting = isExpecting;
                cloned->choices = choices;
                return cloned;
            }

            std::string text;                  // Text content
            std::string language;              // Language code (e.g., "en-US")
            bool isFinal = true;               // false = mid-stream chunk; true = last chunk of a response
            bool isExpecting = false;          // true = LLM expects a user response (set on final chunk only)
            std::vector<Choice> choices;       // UI choices presented to the user (non-empty when LLM restricts valid responses)

            TextFrame() : language("en-US") {}
        };

        /**
         * @brief Tool call frame carrying a parsed tool invocation from the LLM.
         *        args is stored as a serialised JSON string to avoid pulling
         *        nlohmann/json into every translation unit that includes Frame.h.
         */
        class VASDK_EXPORT ToolCallFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::TOOL_CALL; }

            std::shared_ptr<Frame> clone() const override {
                auto c = std::make_shared<ToolCallFrame>();
                c->timestamp  = timestamp;
                c->sequenceId = sequenceId;
                c->frameId    = frameId;
                c->sourceId   = sourceId;
                c->toolName   = toolName;
                c->argsJson   = argsJson;
                return c;
            }

            std::string toolName;  // Name of the tool to invoke
            std::string argsJson;  // Serialised JSON object of arguments
        };

        class VASDK_EXPORT ToolResultFrame : public Frame {
        public:
            FrameType getType() const override { return FrameType::TOOL_RESULT; }

            std::shared_ptr<Frame> clone() const override {
                auto c = std::make_shared<ToolResultFrame>();
                c->timestamp  = timestamp;
                c->sequenceId = sequenceId;
                c->frameId    = frameId;
                c->sourceId   = sourceId;
                c->toolName   = toolName;
                c->resultJson = resultJson;
                return c;
            }

            std::string toolName;   // Name of the tool that was invoked
            std::string resultJson; // Serialised JSON result from the handler
        };

        /**
         * @brief VAD state frame for voice activity detection events
         */
        enum class VADState {
            QUIET,      // No speech detected
            STARTING,   // Speech is starting
            ONGOING,    // Speech is ongoing
            PAUSED,     // Speech paused (hesitation)
            RESUMING,   // Speech is resuming after pause
            ENDING,     // Speech is ending
            ENDED       // Speech has ended
        };

        inline std::string toString(VADState state) {
            switch (state) {
            case VADState::QUIET:    return "QUIET";
            case VADState::STARTING: return "STARTING";
            case VADState::ONGOING:  return "ONGOING";
            case VADState::PAUSED:   return "PAUSED";
            case VADState::RESUMING: return "RESUMING";
            case VADState::ENDING:   return "ENDING";
            case VADState::ENDED:    return "ENDED";
            default:                 return "UNKNOWN";
            }
        }

        class VASDK_EXPORT VADStateFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::VAD_STATE; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<VADStateFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->state = state;
                cloned->speechStartOffset = speechStartOffset;
                cloned->speechEndOffset = speechEndOffset;
                cloned->totalSpeechLength = totalSpeechLength;
                cloned->confidence = confidence;
                return cloned;
            }

            VADState state;              // Current VAD state
            uint64_t speechStartOffset;  // Start position in audio stream
            uint64_t speechEndOffset;    // End position in audio stream
            uint64_t totalSpeechLength;  // Total length of detected speech
            float confidence;            // Confidence score (0.0 - 1.0)

            VADStateFrame()
                : state(VADState::QUIET)
                , speechStartOffset(0)
                , speechEndOffset(0)
                , confidence(0.0f)
                , totalSpeechLength(0) {
            }
        };

        /**
         * @brief Turn state frame for dialog turn management
         */
        class VASDK_EXPORT TurnStateFrame : public Frame {
            public:

            /**
             * @brief Turn actor - who is taking the turn in the conversation
             */
            enum class TurnActor {
                USER,
                SYSTEM,
                OTHER
            };

            /**
             * @brief Turn state - whether a turn is starting or ending
             */
            enum class TurnState {
                START,
                END
            };

            /**
             * @brief Turn type - categorization of the turn (e.g., normal, interrupt, overlap)
             */
            enum class TurnType {
                NORMAL,
                INTERRUPT,
                OVERLAP
            };

            FrameType getType() const override { return FrameType::TURN_STATE; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<TurnStateFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->actor = actor;
                cloned->state = state;
                cloned->type = type;
                cloned->confidence = confidence;
                return cloned;
            }

            TurnActor actor;   // Actor of the turn            
            TurnState state;   // State of the turn (start/end)
            TurnType type;     // Type of turn (normal, interrupt, overlap)
            float confidence;  // Confidence score for turn detection

            TurnStateFrame() = default;
            TurnStateFrame(TurnActor turnActor, TurnState turnState, TurnType turnType, float confidence = 1.0f)
                : actor(turnActor), confidence(confidence), state(turnState), type(turnType) {}
        };

        /**
         * @brief Transcription frame for STT results
         */
        class VASDK_EXPORT TranscriptionFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::TRANSCRIPTION; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<TranscriptionFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->transcriptionResult = transcriptionResult;
                cloned->isFinal = isFinal;
                return cloned;
            }

            bool isFinal;              // Whether this is a final transcription result
            va::audio::TranscriptionResult transcriptionResult; // Full result with word-level data, alternatives, and timing

            TranscriptionFrame() = default;
        };

        /**
         * @brief Synthesis frame for TTS results
         */
        class VASDK_EXPORT SynthesisFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::SYNTHESIS; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<SynthesisFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->text = text;
                cloned->audioData = audioData;
                cloned->voice = voice;
                return cloned;
            }

            std::string text;                // Text that was synthesized
            std::vector<char> audioData;  // Synthesized audio
            std::string voice;               // Voice ID used

            SynthesisFrame() : voice("default") {}
        };

        /**
         * @brief Trigger frame for wake word or button events
         */
        enum class TriggerType {
            WAKE_WORD,
            BUTTON_PRESS,
            APP_COMMAND,
            SCHEDULED
        };

        class VASDK_EXPORT TriggerFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::TRIGGER; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<TriggerFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->triggerType = triggerType;
                cloned->triggerData = triggerData;
                cloned->confidence = confidence;
                return cloned;
            }

            TriggerType triggerType;   // Type of trigger
            std::string triggerData;   // Additional data (e.g., wake word detected)
            float confidence;          // Confidence score

            TriggerFrame()
                : triggerType(TriggerType::WAKE_WORD)
                , confidence(0.0f) {
            }
        };

        /**
         * @brief Control frame for pipeline control
         */
        enum class ControlCommand {
            START,
            STOP,
            PAUSE,
            RESUME,
            RESET,
            INTERRUPT
        };

        class VASDK_EXPORT ControlFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::CONTROL; }

            FramePriority getPriority() const override {
                // Interrupts are critical
                return (command == ControlCommand::INTERRUPT)
                    ? FramePriority::CRITICAL
                    : FramePriority::HIGH;
            }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<ControlFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->command = command;
                cloned->targetId = targetId;
                return cloned;
            }

            ControlCommand command;  // Control command
            std::string targetId;    // Target component ID (empty = all)

            ControlFrame() : command(ControlCommand::START) {}
        };

        /**
         * @brief Specialized control frame for signaling end of audio source (e.g., end of file or stream)
         */
        class AudioSourceEndControlFrame : public ControlFrame {
            public:
            AudioSourceEndControlFrame() {
                command = ControlCommand::INTERRUPT;
            }

            FramePriority getPriority() const override {
                return FramePriority::LOW;
            }
        };

        /**
         * @brief Metadata frame for session/context information
         */
        class VASDK_EXPORT MetadataFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::METADATA; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<MetadataFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->key = key;
                cloned->value = value;
                return cloned;
            }

            std::string key;    // Metadata key
            std::string value;  // Metadata value
        };

        /**
         * @brief Error frame for error reporting
         */
        enum class ErrorSeverity {
            WARNING,
            ERROR,
            FATAL
        };

        class VASDK_EXPORT ErrorFrame : public Frame {
            public:
            FrameType getType() const override { return FrameType::ERROR; }

            FramePriority getPriority() const override {
                return (severity == ErrorSeverity::FATAL)
                    ? FramePriority::CRITICAL
                    : FramePriority::HIGH;
            }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<ErrorFrame>();
                cloned->timestamp = timestamp;
                cloned->sequenceId = sequenceId;
                cloned->frameId = frameId;
                cloned->sourceId = sourceId;
                cloned->severity = severity;
                cloned->errorCode = errorCode;
                cloned->errorMessage = errorMessage;
                return cloned;
            }

            ErrorSeverity severity;    // Error severity
            int errorCode;             // Error code
            std::string errorMessage;  // Human-readable error message

            ErrorFrame()
                : severity(ErrorSeverity::ERROR)
                , errorCode(0) {
            }
        };

        /**
         * @brief Synthesis state frame - TTS lifecycle events emitted upstream by TTSFrameProcessor.
         *
         * Flows UPSTREAM so that AgentStateProcessor and TurnDetectionProcessor can
         * react to TTS activity without requiring manual callback wiring in main_pipeline.cpp.
         */
        enum class SynthesisState {
            STARTED,      // TTS synthesis has begun
            COMPLETED,    // TTS synthesis finished normally
            INTERRUPTED,  // TTS synthesis was force-stopped (e.g. barge-in)
            ERROR         // TTS synthesis failed
        };

        inline std::string toString(SynthesisState state) {
            switch (state) {
            case SynthesisState::STARTED:     return "STARTED";
            case SynthesisState::COMPLETED:   return "COMPLETED";
            case SynthesisState::INTERRUPTED: return "INTERRUPTED";
            case SynthesisState::ERROR:       return "ERROR";
            }
            return "UNKNOWN";
        }

        class VASDK_EXPORT SynthesisStateFrame : public Frame {
        public:
            FrameType getType() const override { return FrameType::SYNTHESIS_STATE; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<SynthesisStateFrame>();
                cloned->timestamp          = timestamp;
                cloned->sequenceId         = sequenceId;
                cloned->frameId            = frameId;
                cloned->sourceId           = sourceId;
                cloned->state              = state;
                cloned->errorMessage       = errorMessage;
                return cloned;
            }

            SynthesisState state{ SynthesisState::STARTED };
            std::string    errorMessage;  // populated on ERROR only
        };

        /**
         * @brief Agent state frame - emitted downstream by AgentStateProcessor on every
         *        state transition.  Consumed by LLMProcessor and TTSFrameProcessor to
         *        determine whether to cancel in-flight operations.
         */
        enum class AgentState {
            IDLE,         // Pipeline running but agent dormant (wake-word / deactivated)
            LISTENING,    // Ready to accept user speech
            THINKING,     // Transcription done; waiting for LLM response
            SPEAKING,     // TTS is playing agent response
            INTERRUPTED   // Transition state: in-flight operation was cancelled
        };

        enum class AgentStateTransition {
            PIPELINE_START,
            WAKE_WORD_DETECTED,
            APP_COMMAND,
            USER_STARTED_SPEAKING,
            USER_STOPPED_SPEAKING,
            TRANSCRIPTION_READY,
            SYNTHESIS_STARTED,
            SYNTHESIS_COMPLETED,
            BARGE_IN_INTERRUPT,
            USER_SPOKE_WHILE_THINKING,
            INTERRUPT_RESOLVED
        };

        inline std::string toString(AgentState state) {
            switch (state) {
            case AgentState::IDLE:        return "IDLE";
            case AgentState::LISTENING:   return "LISTENING";
            case AgentState::THINKING:    return "THINKING";
            case AgentState::SPEAKING:    return "SPEAKING";
            case AgentState::INTERRUPTED: return "INTERRUPTED";
            }
            return "UNKNOWN";
        }

        inline std::string toString(AgentStateTransition t) {
            switch (t) {
            case AgentStateTransition::PIPELINE_START:            return "PIPELINE_START";
            case AgentStateTransition::WAKE_WORD_DETECTED:        return "WAKE_WORD_DETECTED";
            case AgentStateTransition::APP_COMMAND:               return "APP_COMMAND";
            case AgentStateTransition::USER_STARTED_SPEAKING:     return "USER_STARTED_SPEAKING";
            case AgentStateTransition::USER_STOPPED_SPEAKING:     return "USER_STOPPED_SPEAKING";
            case AgentStateTransition::TRANSCRIPTION_READY:       return "TRANSCRIPTION_READY";
            case AgentStateTransition::SYNTHESIS_STARTED:         return "SYNTHESIS_STARTED";
            case AgentStateTransition::SYNTHESIS_COMPLETED:       return "SYNTHESIS_COMPLETED";
            case AgentStateTransition::BARGE_IN_INTERRUPT:        return "BARGE_IN_INTERRUPT";
            case AgentStateTransition::USER_SPOKE_WHILE_THINKING: return "USER_SPOKE_WHILE_THINKING";
            case AgentStateTransition::INTERRUPT_RESOLVED:        return "INTERRUPT_RESOLVED";
            }
            return "UNKNOWN";
        }

        class VASDK_EXPORT AgentStateFrame : public Frame {
        public:
            FrameType getType() const override { return FrameType::AGENT_STATE; }

            std::shared_ptr<Frame> clone() const override {
                auto cloned = std::make_shared<AgentStateFrame>();
                cloned->timestamp         = timestamp;
                cloned->sequenceId        = sequenceId;
                cloned->frameId           = frameId;
                cloned->sourceId          = sourceId;
                cloned->currentState      = currentState;
                cloned->previousState     = previousState;
                cloned->transition        = transition;
                cloned->triggerConfidence = triggerConfidence;
                return cloned;
            }

            AgentState           currentState { AgentState::IDLE };
            AgentState           previousState{ AgentState::IDLE };
            AgentStateTransition transition   { AgentStateTransition::PIPELINE_START };
            /// Confidence of the event that triggered this transition (0–1).
            /// For USER_STOPPED_SPEAKING this is the turn-detection model probability.
            float                triggerConfidence{ 0.0f };
        };

    } // namespace transport
} // namespace va
