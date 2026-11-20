#pragma once

#include "nlu/Language.h"
#include "BargeInClassifier.h"
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
#include <optional>
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
            /// What may be said over the agent without interrupting it ("yeah", "mm-hmm"). Empty: the
            /// languages' back-channels (nlu::Language's turn_taking; useLanguageWords).
            std::vector<std::string> backchannelPhrases;
            /// Any of these words makes the speech a real interruption at once ("okay stop", "what?": not
            /// heard). Empty: the languages' interruptions.
            std::vector<std::string> interruptWords;
            /// Speech over the agent that is only its own words (speakers heard by the mic, with no echo
            /// cancellation) doesn't interrupt it. Needs the words being spoken (setSpokenTextProvider).
            bool rejectEcho = true;
            /// The share of the words heard that must be the reply's for them to be its echo (2 words or more).
            float echoMinShare = 0.8f;
            /// Speech past commitAfterPendingMs with no words yet waits this much longer for them, echo or not.
            /// 0: it interrupts then, as without rejectEcho. Raise it (600) for speakers with no echo
            /// cancellation and an STT whose words come late; every interruption then waits as long.
            uint32_t echoWordsWaitMs = 0;
            /// Who decides: the word rules only (today's behavior), a model when the rules are unsure,
            /// or a model for all speech with words. A model mode needs a classifier
            /// (AgentStateProcessor::setBargeInClassifier); without one, the word rules decide.
            /// An agent's page or part can choose its own (TextFrame::bargeInDecision).
            transport::BargeInDecisionMode decisionMode = transport::BargeInDecisionMode::WordRulesOnly;
            /// How long a decision waits for the model, in audio time past the point the word rules
            /// would have decided (commitAfterPendingMs, finalTranscriptionWaitMs); then the rules decide.
            uint32_t modelTimeoutMs = 800;
            /// Judge speech while THINKING too: an acknowledgement or a noise ("okay", a cough) keeps the
            /// coming reply (paused until decided); words that add to the request cancel it, as without
            /// this. false: any speech while THINKING cancels the reply at once.
            bool judgeWhileThinking = false;
        };

        /** Fills `config`'s empty phrase and word lists with `languages`' back-channels and interruptions. */
        VASDK_EXPORT void useLanguageWords(BackchannelConfig& config, const nlu::Languages& languages);

        /**
         * @brief Classifies a transcript heard over the agent. Punctuation and case are ignored, and
         *        phrases match as whole words. A partial is Undecided until it can only be an interruption.
         *        confidence is the STT confidence (0-1) of a final; see minInterruptConfidence.
         */
        VASDK_EXPORT BackchannelVerdict classifyBackchannel(const std::string& text, bool isFinal,
                                                            const BackchannelConfig& backchannelConfig,
                                                            float confidence = 1.0f);

        /**
         * @brief Whether `heard` is the agent's own `spoken` words coming back through the mic: 2
         *        words or more, at least echoMinShare of them in the reply, and no interrupt word
         *        ("wait, the second one" interrupts even when the reply said "second one").
         */
        VASDK_EXPORT bool isEcho(const std::string& heard, const std::string& spoken, const BackchannelConfig& backchannelConfig);

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
             * Thread-safe; applies from the next reply. An agent's reply can set its own window
             * (FollowUpListenFrame, sent upstream by the dialog stage), for that reply only.
             */
            void setFollowUpListenMs(int ms) { _followUpListenMs = ms; }

            /**
             * @brief Timeout before forcing INTERRUPTED → LISTENING if TTS does not
             *        emit SynthesisStateFrame{INTERRUPTED}.  Default: 500ms.
             */
            void setInterruptTimeoutMs(uint32_t ms) { _interruptTimeoutMs = ms; }

            /// Back-channel policy; enabled = false restores the plain hard barge-in. Before start(), or
            /// any time from another thread: it then applies from the next frame. Empty phrase and
            /// word lists are the languages' (setLanguages).
            void setBackchannelConfig(const BackchannelConfig& backchannelConfig) {
                std::lock_guard<std::mutex> lock(_pendingBackchannelMutex);
                _pendingBackchannel = backchannelConfig;
                useLanguageWords(*_pendingBackchannel, _languages);
            }
            /// The languages the user may speak over the agent in: their back-channels ("yeah",
            /// "ouais") and interruptions, for a policy that lists none. Default: VASDK's built-in en-US.
            void setLanguages(const nlu::Languages& languages) {
                std::lock_guard<std::mutex> lock(_pendingBackchannelMutex);
                _languages = languages;
            }
            /// The policy in use, with any setBackchannelConfig() not yet applied.
            BackchannelConfig getBackchannelConfig() const {
                std::lock_guard<std::mutex> lock(_pendingBackchannelMutex);
                return _pendingBackchannel ? *_pendingBackchannel : _backchannelConfig;
            }

            /**
             * @brief Milliseconds of the agent's reply still to play, or a negative value when unknown
             *        (e.g. TTSFrameProcessor::getRemainingReplyPlaybackMs). Enables the tail-overlap rule.
             */
            void setPlaybackRemainingMsProvider(std::function<int()> playbackRemainingMsProvider) {
                _playbackRemainingMsProvider = std::move(playbackRemainingMsProvider);
            }

            /**
             * @brief The words of the reply being spoken, or the one just finished
             *        (TTSFrameProcessor::currentReplyText): speech that is only them is echo (rejectEcho).
             */
            void setSpokenTextProvider(std::function<std::string()> spokenTextProvider) {
                _spokenTextProvider = std::move(spokenTextProvider);
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

            /**
             * @brief What decides on speech over the agent when BackchannelConfig::decisionMode (or the
             *        playing part's mode) asks a model: called on a worker thread of this processor's own,
             *        one call at a time. nullptr: the word rules decide. Before start(), or any time.
             */
            void setBargeInClassifier(std::shared_ptr<const BargeInClassifier> bargeInClassifier) {
                std::lock_guard<std::mutex> lock(_modelMutex);
                _bargeInClassifier = std::move(bargeInClassifier);
            }

            /**
             * @brief The decision mode of the part of the reply playing now, when it chose one
             *        (TTSFrameProcessor::getPlayingPartDecision); it wins over the config for an overlap
             *        that starts while it plays.
             */
            void setPlayingPartDecisionProvider(std::function<std::optional<transport::BargeInDecisionMode>()> provider) {
                _playingPartDecisionProvider = std::move(provider);
            }

            /**
             * @brief The words of the reply being spoken with the user's private values masked
             *        (TTSFrameProcessor::currentReplyText(true)): what a BargeInClassifier is told.
             *        Unset: it's told the spoken text (setSpokenTextProvider).
             */
            void setMaskedReplyTextProvider(std::function<std::string()> maskedReplyTextProvider) {
                _maskedReplyTextProvider = std::move(maskedReplyTextProvider);
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
            mutable std::mutex _pendingBackchannelMutex;
            std::optional<BackchannelConfig> _pendingBackchannel;   // applied on the processing thread
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

            // Asking the BargeInClassifier, on _modelThread
            struct ModelRequest {
                uint64_t overlapGeneration = 0;
                bool isFinal = false;   // the user stopped: the verdict settles the overlap
                BargeInContext context;
            };
            void askModel(bool isFinal);
            void modelLoop();
            void applyModelVerdict(const ModelRequest& request, BackchannelVerdict verdict);

            // Back-channel handling (pending barge-in while SPEAKING)
            enum class FinalAfterSpeakingMode { None, ReplayAsTurn, ClassifyThenReplay };
            // PendingWhileThinking: speech while THINKING (judgeWhileThinking), decided like a pending barge-in.
            enum class OverlapKind { None, PendingBargeIn, PendingWhileThinking, TailTurn };
            static bool isPending(OverlapKind overlapKind) {
                return overlapKind == OverlapKind::PendingBargeIn || overlapKind == OverlapKind::PendingWhileThinking;
            }
            void beginOverlap(OverlapKind overlapKind);
            void handleOverlapAudio(const transport::AudioFrame& audioFrame);
            void handleOverlapUserTurnEnded();
            void handleOverlapTranscription(std::shared_ptr<transport::TranscriptionFrame> frame);
            void handleOverlapSystemTurnEnded();
            void decideOverlap();
            // onlyGeneration: act only on that overlap (a model's verdict may come late); 0: the current one.
            void decideOverlapByRules(uint64_t onlyGeneration = 0);
            bool overlapEchoes(const std::string& heard, OverlapKind overlapKind, const BackchannelConfig& backchannelConfig) const;
            void commitBargeIn(bool userSpeechEnded, const char* reason, uint64_t onlyGeneration = 0);
            void dismissBackchannel(uint64_t onlyGeneration = 0);
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
            transport::FollowUpListenFrame _nextReplyWindow;   // the reply being spoken's own window; under _followUpMutex
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
            nlu::Languages       _languages{{nlu::Language::builtIn()}};
            std::function<int()> _playbackRemainingMsProvider;
            std::function<std::string()> _spokenTextProvider;
            bool echoes(const std::string& heard) const;
            std::function<int()> _protectedPlaybackMsProvider;
            std::function<std::optional<transport::BargeInDecisionMode>()> _playingPartDecisionProvider;
            std::function<std::string()> _maskedReplyTextProvider;
            std::atomic<bool> _privateAnswer{ false };   // a private answer is expected: never sent to a model

            // Model worker thread
            std::thread             _modelThread;
            std::mutex              _modelMutex;
            std::condition_variable _modelCv;
            std::shared_ptr<const BargeInClassifier> _bargeInClassifier;
            std::optional<ModelRequest> _modelQueue;   // the newest question replaces an unasked one
            bool                    _modelStop{ false };
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
            uint64_t    _overlapGeneration{ 0 };       // a new one per overlap: older model verdicts are dropped
            transport::BargeInDecisionMode _overlapDecisionMode{ transport::BargeInDecisionMode::WordRulesOnly };
            bool        _modelPending{ false };        // a question to the model is queued or in flight
            std::string _modelAskedText;              // what it was asked, so the same words aren't asked twice
            bool        _modelAskedFinal{ false };
            uint32_t    _modelAskedAtMs{ 0 };          // _pendingSpeechDurationMs when it was asked: its time runs from there
            std::string _modelBackchannelText;        // the words the model last called a back-channel
            std::string _lastUserTurn;                // the user's last turn, for the model's context
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
                va::metrics::CounterHandle modelVerdicts;
                va::metrics::SamplerHandle modelLatencyMs;
                va::metrics::CounterHandle repliesKeptWhileThinking;
            } _metrics;
            uint64_t _utteranceStartNs{0};
        };

    } // namespace pipeline
} // namespace va
