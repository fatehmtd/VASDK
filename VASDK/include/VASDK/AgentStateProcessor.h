#pragma once

#include "FrameProcessor.h"
#include "Frame.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include "metrics/UtteranceObserver.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace va {
    namespace pipeline {

        /// What happens to a dismissed back-channel ("mm-hmm") transcript.
        enum class BackchannelHistoryMode {
            Drop,             // never reaches the LLM
            AppendToHistory   // forwarded with TranscriptionFrame::isBackchannel; recorded, not answered
        };

        /// User speech over the last tailOverlapMs of the agent's reply.
        enum class TailOverlapMode {
            TreatAsTurn,  // let the reply finish, then answer it ("yes" to "shall I book it?")
            Classify      // classify like any other overlap; back-channels are dismissed
        };

        enum class BackchannelVerdict { Backchannel, Interrupt, Undecided };

        /**
         * @brief Back-channel handling: user speech while the agent is SPEAKING is held as a
         *        pending barge-in until it is classified as a back-channel (dismissed, the agent
         *        keeps talking) or a real interruption (today's hard barge-in).
         */
        struct BackchannelConfig {
            bool enabled = true;
            transport::PendingPlaybackMode pendingPlaybackMode = transport::PendingPlaybackMode::Duck;
            BackchannelHistoryMode historyMode = BackchannelHistoryMode::Drop;
            TailOverlapMode tailOverlapMode = TailOverlapMode::TreatAsTurn;
            /// Playback level while ducked (PendingPlaybackMode::Duck).
            float duckGainDecibels = -12.0f;
            /// The duck lasts at least this long from the start of the overlap, so a back-channel
            /// doesn't bring the volume straight back up (TTSFrameProcessor::setPendingPlayback).
            uint32_t duckDurationMs = 2500;
            /// Pending speech longer than this is a real interruption (~850 ms of speech with the VAD confirm).
            uint32_t commitAfterPendingMs = 600;
            /// After the user stops, wait this long for a final transcript before deciding on the latest partial.
            uint32_t finalTranscriptionWaitMs = 300;
            /// Less reply audio left than this: overlapping speech is the user's turn (TailOverlapMode::TreatAsTurn).
            uint32_t tailOverlapMs = 700;
            /// "yeah yeah okay right" is still a back-channel; more words are not.
            uint32_t maxBackchannelWords = 4;
            /// A final of 1-2 words below this STT confidence, with no interrupt word, is treated as a
            /// back-channel: short words heard over the agent's own voice are often misrecognised
            /// ("Hey, hey." for "yeah, yeah"), and a garbled word shouldn't cut the agent off.
            float minInterruptConfidence = 0.6f;
            uint32_t lowConfidenceMaxWords = 2;
            std::vector<std::string> backchannelPhrases{ "yeah", "yep", "yes", "yup", "mm-hmm", "mhm", "mm", "hmm",
                "uh-huh", "okay", "ok", "right", "all right", "alright", "sure", "i see", "got it", "cool", "nice", "wow", "ah", "oh" };
            /// Any of these words makes the speech a real interruption at once ("okay stop").
            std::vector<std::string> interruptWords{ "stop", "wait", "no", "hold", "hang", "actually", "but",
                "sorry", "excuse" };
        };

        /**
         * @brief Classifies a transcript heard over the agent. Punctuation and case are ignored, and
         *        phrases match as whole words. A partial is Undecided until it can only be an interruption.
         *        confidence is the STT confidence (0-1) of a final; see minInterruptConfidence.
         */
        VASDK_EXPORT BackchannelVerdict classifyBackchannel(const std::string& text, bool isFinal,
                                                            const BackchannelConfig& backchannelConfig,
                                                            float confidence = 1.0f);

        /**
         * @brief Agent state processor - central state machine for the voice pipeline.
         *
         * Positioned between TranscriptionFrameProcessor and LLMProcessor:
         *   TurnDetection → Transcription → AgentState → LLM → TTS
         *
         * Responsibilities:
         *  - Tracks conversation state: IDLE, LISTENING, THINKING, SPEAKING, INTERRUPTED
         *  - Emits AgentStateFrame downstream on every transition so LLM and TTS can
         *    react (e.g. cancel in-flight work) without manual callback wiring
         *  - Gates stale TranscriptionFrames: drops them when not in LISTENING state
         *  - Reacts to TriggerFrame (WAKE_WORD, APP_COMMAND) to activate from IDLE
         *  - All transitions driven by TurnStateFrame (USER and SYSTEM actors)
         *  - Maintains a short timeout fallback for the INTERRUPTED state in case TTS
         *    does not emit SynthesisStateFrame{INTERRUPTED}
         *
         * Always-on mode (default): IDLE → LISTENING automatically on start().
         * Wake-word mode: remains IDLE until TriggerFrame{WAKE_WORD/APP_COMMAND/SCHEDULED} arrives.
         * Push-to-talk: TriggerFrame{PUSH_TO_TALK, "down"} listens; "up" stops unless the user is mid-turn.
         * After each reply, setFollowUpListenMs() keeps the mic open for an answer without a trigger.
         *
         * See docs/reference/processors.md for the full state machine diagram.
         */
        class VASDK_EXPORT AgentStateProcessor : public FrameProcessor {
        public:
            AgentStateProcessor();
            ~AgentStateProcessor() override;

            std::string getName() const override { return "AgentStateProcessor"; }
            bool initialize() override;
            bool start() override;
            void stop() override;

            /**
             * @brief Set always-on mode.
             *
             * true (default): IDLE → LISTENING automatically on start().
             * false: agent stays IDLE until a TriggerFrame{WAKE_WORD/APP_COMMAND} arrives.
             */
            void setAlwaysOn(bool alwaysOn) { _alwaysOn = alwaysOn; }

            /**
             * @brief How long the mic stays open after every reply, in ms (follow-up listening).
             *
             * Starts once the reply has played out, or was interrupted. Speech that starts within it
             * is a new turn, no wake word or button needed; it cancels the window. When nobody speaks,
             * the agent goes IDLE (always-on: stays LISTENING), with FOLLOW_UP_WINDOW_CLOSED.
             * 0 (default): no window, as before: IDLE straight away unless always-on. -1: never closes.
             * Thread-safe; applies from the next reply.
             */
            void setFollowUpListenMs(int ms) { _followUpListenMs = ms; }

            /**
             * @brief Timeout before forcing INTERRUPTED → LISTENING if TTS does not
             *        emit SynthesisStateFrame{INTERRUPTED}.  Default: 500ms.
             */
            void setInterruptTimeoutMs(uint32_t ms) { _interruptTimeoutMs = ms; }

            /// Back-channel policy. Set before start(); enabled = false restores the plain hard barge-in.
            void setBackchannelConfig(const BackchannelConfig& backchannelConfig) { _backchannelConfig = backchannelConfig; }
            const BackchannelConfig& getBackchannelConfig() const { return _backchannelConfig; }

            /**
             * @brief Milliseconds of the agent's reply still to play, or a negative value when unknown
             *        (e.g. TTSFrameProcessor::getRemainingReplyPlaybackMs). Enables the tail-overlap rule.
             */
            void setPlaybackRemainingMsProvider(std::function<int()> playbackRemainingMsProvider) {
                _playbackRemainingMsProvider = std::move(playbackRemainingMsProvider);
            }

            /**
             * @brief Milliseconds left of a part of the reply the user can't interrupt, or 0
             *        (TTSFrameProcessor::getProtectedPlaybackMs). Speech that starts, or would cut
             *        the reply, while it plays doesn't: it's answered after the reply, like speech
             *        over the reply's end.
             */
            void setProtectedPlaybackMsProvider(std::function<int()> protectedPlaybackMsProvider) {
                _protectedPlaybackMsProvider = std::move(protectedPlaybackMsProvider);
            }

            transport::AgentState getState() const { return _currentState.load(); }

            void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

            void setMetricsConfig(const va::metrics::ProcessorMetricsConfig& config);
            void setUtteranceObserver(std::shared_ptr<va::metrics::UtteranceObserverInterface> observer) { _utteranceObserver = observer; }

        protected:
            /**
             * DOWNSTREAM: TURN_STATE, TRANSCRIPTION, TRIGGER
             * UPSTREAM:   SYNTHESIS_STATE (pass-through only - TurnDetection converts to TurnStateFrame{SYSTEM})
             */
            bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;

        private:
            void transitionTo(transport::AgentState next, transport::AgentStateTransition reason,
                              float confidence = 0.0f, bool emitWhenUnchanged = false, int listenWindowMs = 0);
            void emitAgentStateFrame(transport::AgentState current,
                                     transport::AgentState previous,
                                     transport::AgentStateTransition reason,
                                     float confidence = 0.0f, int listenWindowMs = 0);

            void handleTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleUserTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleUserTurnStarted(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleUserTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleSystemTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleSystemTurnStarted(std::shared_ptr<transport::TurnStateFrame> frame);
            void handleSystemTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame);

            void handleTranscriptionFrame(std::shared_ptr<transport::TranscriptionFrame> frame);
            void handleTriggerFrame(std::shared_ptr<transport::TriggerFrame> frame);
            void handleAppCommandTrigger(std::shared_ptr<transport::TriggerFrame> frame);
            void handleWakeWordTrigger(std::shared_ptr<transport::TriggerFrame> frame);
            void handleButtonPressTrigger(std::shared_ptr<transport::TriggerFrame> frame);
            void handlePushToTalkTrigger(std::shared_ptr<transport::TriggerFrame> frame);
            void handleSynthesisStateFrame(std::shared_ptr<transport::SynthesisStateFrame> frame);

            void startInterruptTimeout();
            void cancelInterruptTimeout();
            void interruptTimeoutLoop();

            // Back-channel handling (pending barge-in while SPEAKING)
            enum class FinalAfterSpeakingMode { None, ReplayAsTurn, ClassifyThenReplay };
            enum class OverlapKind { None, PendingBargeIn, TailTurn };
            void beginOverlap(OverlapKind overlapKind);
            void handleOverlapAudio(const transport::AudioFrame& audioFrame);
            void handleOverlapUserTurnEnded();
            void handleOverlapTranscription(std::shared_ptr<transport::TranscriptionFrame> frame);
            void handleOverlapSystemTurnEnded();
            void decideOverlap();
            void commitBargeIn(bool userSpeechEnded, const char* reason);
            void dismissBackchannel();
            void clearOverlapLocked();
            std::string overlapTextLocked() const;
            std::shared_ptr<transport::TranscriptionFrame> takeOverlapTranscriptionLocked(bool includePartial);
            void replayHeldTurn(std::shared_ptr<transport::TranscriptionFrame> heldTranscription);
            void resolveInterrupt();
            // After a reply ends or is interrupted: LISTENING with a follow-up window, or IDLE.
            void finishReply(transport::AgentStateTransition reason);
            void armFollowUpWindow(int ms);
            void cancelFollowUpWindow();
            void followUpLoop();

            std::atomic<transport::AgentState> _currentState{ transport::AgentState::IDLE };
            bool _alwaysOn{ true };
            uint32_t _interruptTimeoutMs{ 500 };
            std::atomic<int> _followUpListenMs{ 0 };
            std::atomic<bool> _pushToTalkHeld{ false };
            std::atomic<bool> _userSpeaking{ false };   // between the user's turn start and end

            // Follow-up window timer
            std::thread             _followUpThread;
            std::mutex              _followUpMutex;
            std::condition_variable _followUpCv;
            std::chrono::steady_clock::time_point _followUpDeadline;
            bool                    _followUpArmed{ false };   // a timed window is counting down
            bool                    _followUpOpen{ false };    // a window is open, timed or not
            bool                    _followUpStop{ false };

            // Interrupt timeout fallback thread
            std::thread             _interruptTimeoutThread;
            std::mutex              _interruptMutex;
            std::condition_variable _interruptCv;
            bool                    _interruptPending{ false };
            bool                    _interruptStop{ false };

            BackchannelConfig    _backchannelConfig;
            std::function<int()> _playbackRemainingMsProvider;
            std::function<int()> _protectedPlaybackMsProvider;
            bool playbackProtected() const { return _protectedPlaybackMsProvider && _protectedPlaybackMsProvider() > 0; }
            // Guards the overlap fields below: the interrupt-timeout thread also resolves interrupts.
            std::mutex  _backchannelMutex;
            OverlapKind _overlapKind{ OverlapKind::None };
            bool        _userSpeechEnded{ false };
            uint32_t    _pendingSpeechDurationMs{ 0 };
            uint32_t    _audioDurationSinceSpeechEndMs{ 0 };
            std::string _overlapFinalText;            // finals heard during the overlap
            float       _overlapFinalConfidence{ 0.0f };
            std::string _latestPartialTranscription;  // newest partial after those finals
            // Late commit: the user's turn, replayed to the LLM once the interrupt resolves.
            std::shared_ptr<transport::TranscriptionFrame> _heldFinalTranscription;
            bool        _replayTurnAfterInterruptResolved{ false };
            // The reply ended after the user stopped but before their final arrived.
            FinalAfterSpeakingMode _finalAfterSpeakingMode{ FinalAfterSpeakingMode::None };

            // Metrics
            std::weak_ptr<va::metrics::UtteranceObserverInterface> _utteranceObserver;
            struct Metrics {
                va::metrics::CounterHandle transitions;
                va::metrics::CounterHandle backchannels;
                va::metrics::CounterHandle bargeInsCommitted;
            } _metrics;
            uint64_t _utteranceStartNs{0};
        };

    } // namespace pipeline
} // namespace va
