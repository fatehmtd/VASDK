#include "VASDK/AgentStateProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include "AgentStateProcessor.h"

namespace va {
    namespace pipeline {

        namespace {
            // Lowercase, punctuation to spaces (keeping word-internal hyphens), common spellings unified.
            std::vector<std::string> normalizedWords(const std::string& text) {
                std::string cleaned;
                cleaned.reserve(text.size());
                for (size_t index = 0; index < text.size(); ++index) {
                    const unsigned char character = static_cast<unsigned char>(text[index]);
                    if (std::isalnum(character) || character >= 0x80) {
                        cleaned += static_cast<char>(std::tolower(character));
                    } else if (character == '\'') {
                        // "that's" -> "thats"
                    } else if (character == '-' && !cleaned.empty() && cleaned.back() != ' ' &&
                               index + 1 < text.size() && std::isalnum(static_cast<unsigned char>(text[index + 1]))) {
                        cleaned += '-';
                    } else {
                        cleaned += ' ';
                    }
                }
                std::vector<std::string> words;
                std::istringstream stream(cleaned);
                for (std::string word; stream >> word;) words.push_back(word);

                std::vector<std::string> unified;
                for (size_t index = 0; index < words.size(); ++index) {
                    const std::string& word = words[index];
                    const std::string next = index + 1 < words.size() ? words[index + 1] : std::string();
                    if ((word == "uh" || word == "un") && (next == "huh" || next == "hu")) { unified.push_back("uh-huh"); ++index; }
                    else if ((word == "mm" || word == "m") && (next == "hmm" || next == "hm")) { unified.push_back("mm-hmm"); ++index; }
                    else if (word == "uhhuh" || word == "uh-hu") unified.push_back("uh-huh");
                    else if (word == "mhmm" || word == "mmhmm" || word == "mm-hm" || word == "mhm-hmm") unified.push_back("mm-hmm");
                    else unified.push_back(word);
                }
                return unified;
            }
        } // namespace

        BackchannelVerdict classifyBackchannel(const std::string& text, bool isFinal,
                                               const BackchannelConfig& backchannelConfig, float confidence)
        {
            const std::vector<std::string> words = normalizedWords(text);
            if (words.empty()) return isFinal ? BackchannelVerdict::Backchannel : BackchannelVerdict::Undecided;

            std::vector<std::vector<std::string>> phrases;
            for (const auto& phrase : backchannelConfig.backchannelPhrases) {
                auto phraseWords = normalizedWords(phrase);
                if (!phraseWords.empty()) phrases.push_back(std::move(phraseWords));
            }
            std::vector<std::string> interruptWords;
            for (const auto& interruptWord : backchannelConfig.interruptWords) {
                for (auto& word : normalizedWords(interruptWord)) interruptWords.push_back(std::move(word));
            }

            uint32_t unitCount = 0;
            uint32_t unknownCount = 0;
            for (size_t index = 0; index < words.size();) {
                if (std::find(interruptWords.begin(), interruptWords.end(), words[index]) != interruptWords.end())
                    return BackchannelVerdict::Interrupt;
                size_t longestMatch = 0;
                for (const auto& phrase : phrases) {
                    if (phrase.size() <= longestMatch || index + phrase.size() > words.size()) continue;
                    if (std::equal(phrase.begin(), phrase.end(), words.begin() + static_cast<std::ptrdiff_t>(index)))
                        longestMatch = phrase.size();
                }
                ++unitCount;
                if (longestMatch == 0) { ++unknownCount; ++index; }
                else index += longestMatch;
            }

            if (unitCount > backchannelConfig.maxBackchannelWords) return BackchannelVerdict::Interrupt;
            // Interrupt words returned above, so a short garble is only ever unknown words.
            if (isFinal && unknownCount > 0 && confidence < backchannelConfig.minInterruptConfidence &&
                unitCount <= backchannelConfig.lowConfidenceMaxWords)
                return BackchannelVerdict::Backchannel;
            if (unknownCount >= 2) return BackchannelVerdict::Interrupt;
            if (unknownCount == 1) return isFinal ? BackchannelVerdict::Interrupt : BackchannelVerdict::Undecided;
            return isFinal ? BackchannelVerdict::Backchannel : BackchannelVerdict::Undecided;
        }

        AgentStateProcessor::AgentStateProcessor() = default;

        AgentStateProcessor::~AgentStateProcessor() {
            stop();
        }

        bool AgentStateProcessor::initialize() {
            if (!FrameProcessor::initialize()) return false;

            if (_baseMetrics.config.enabled && _collector) {
                using namespace va::metrics;
                _metrics.transitions.initialize(_collector,
                    MetricDescriptor{ "agent_transitions_total", MetricType::Counter,
                                     "Total agent state transitions" });
                _metrics.backchannels.initialize(_collector,
                    MetricDescriptor{ "agent_backchannels_total", MetricType::Counter,
                                     "User speech over the agent dismissed as a back-channel" });
                _metrics.bargeInsCommitted.initialize(_collector,
                    MetricDescriptor{ "agent_bargein_committed_total", MetricType::Counter,
                                     "User speech over the agent that interrupted it" });
            }

            return true;
        }

        void AgentStateProcessor::setMetricsConfig(const va::metrics::ProcessorMetricsConfig& config)
        {
            FrameProcessor::setMetricsConfig(config);
        }

        bool AgentStateProcessor::start() {
            if (!FrameProcessor::start()) return false;

            // Launch interrupt timeout thread
            {
                std::lock_guard<std::mutex> lock(_interruptMutex);
                _interruptStop = false;
                _interruptPending = false;
            }
            _interruptTimeoutThread = std::thread(&AgentStateProcessor::interruptTimeoutLoop, this);
            {
                std::lock_guard<std::mutex> lock(_followUpMutex);
                _followUpStop = false;
                _followUpArmed = false;
            }
            _followUpThread = std::thread(&AgentStateProcessor::followUpLoop, this);

            if (_alwaysOn) {
                transitionTo(transport::AgentState::LISTENING,
                    transport::AgentStateTransition::PIPELINE_START);
            }
            return true;
        }

        void AgentStateProcessor::stop() {
            cancelInterruptTimeout();
            {
                std::lock_guard<std::mutex> lock(_interruptMutex);
                _interruptStop = true;
            }
            _interruptCv.notify_all();
            if (_interruptTimeoutThread.joinable()) _interruptTimeoutThread.join();
            {
                std::lock_guard<std::mutex> lock(_followUpMutex);
                _followUpStop = true;
            }
            _followUpCv.notify_all();
            if (_followUpThread.joinable()) _followUpThread.join();
            FrameProcessor::stop();
        }

        //  shouldProcess 

        bool AgentStateProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
            if (!frame) return false;
            switch (frame->getType()) {
            case transport::FrameType::TURN_STATE:
            case transport::FrameType::TRANSCRIPTION:
            case transport::FrameType::TRIGGER:
            case transport::FrameType::SYNTHESIS_STATE:
            case transport::FrameType::AUDIO:
            return true;
            default:
            return false;
            }
        }

        //  processFrame 

        void AgentStateProcessor::processFrame(
            std::shared_ptr<transport::Frame> frame, FlowDirection direction)
        {
            VASDK_CTRACK_NAME("Agent::processFrame");
            if (direction == FlowDirection::UPSTREAM) {
                if (frame->getType() == transport::FrameType::SYNTHESIS_STATE) {
                    handleSynthesisStateFrame(
                        std::dynamic_pointer_cast<transport::SynthesisStateFrame>(frame));
                }
                pushUpstream(frame);
                return;
            }

            // DOWNSTREAM
            switch (frame->getType()) {
            case transport::FrameType::TURN_STATE:
            handleTurnStateFrame(
                std::dynamic_pointer_cast<transport::TurnStateFrame>(frame));
            pushDownstream(frame);
            break;

            case transport::FrameType::TRANSCRIPTION:
            handleTranscriptionFrame(
                std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame));
            // Frame forwarding is conditional - handled inside handleTranscriptionFrame
            break;

            case transport::FrameType::TRIGGER:
            handleTriggerFrame(
                std::dynamic_pointer_cast<transport::TriggerFrame>(frame));
            pushDownstream(frame);
            break;

            case transport::FrameType::AUDIO:
            if (_currentState.load() != transport::AgentState::IDLE) {
                pushDownstream(frame);
            }
            if (_currentState.load() == transport::AgentState::SPEAKING) {
                if (auto audioFrame = std::dynamic_pointer_cast<transport::AudioFrame>(frame))
                    handleOverlapAudio(*audioFrame);
            }
            break;

            default:
            pushDownstream(frame);
            break;
            }
        }

        //  Frame handlers 

        void AgentStateProcessor::handleTurnStateFrame(
            std::shared_ptr<transport::TurnStateFrame> frame)
        {
            switch (frame->actor) {
            case transport::TurnStateFrame::TurnActor::USER:
                handleUserTurnStateFrame(frame);
                break;
            case transport::TurnStateFrame::TurnActor::SYSTEM:
                handleSystemTurnStateFrame(frame);
                break;
            default:
                break;
            }
        }

        void AgentStateProcessor::handleUserTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            switch (frame->state) {
            case transport::TurnStateFrame::TurnState::START:
                handleUserTurnStarted(frame);
                break;
            case transport::TurnStateFrame::TurnState::END:
                handleUserTurnEnded(frame);
                break;
            }
        }

        void AgentStateProcessor::handleUserTurnStarted(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            _userSpeaking = true;
            switch (_currentState.load()) {
            case transport::AgentState::SPEAKING: {
                if (frame->type != transport::TurnStateFrame::TurnType::INTERRUPT) break;
                if (!_backchannelConfig.enabled) {
                    transitionTo(transport::AgentState::INTERRUPTED,
                        transport::AgentStateTransition::BARGE_IN_INTERRUPT);
                    startInterruptTimeout();
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(_backchannelMutex);
                    if (_overlapKind != OverlapKind::None) {
                        // Another stretch of the same overlap ("yeah... yeah"): keep deciding on all of it.
                        _userSpeechEnded = false;
                        _audioDurationSinceSpeechEndMs = 0;
                        break;
                    }
                }
                const int playbackRemainingMs = _playbackRemainingMsProvider ? _playbackRemainingMsProvider() : -1;
                const bool nearReplyEnd = _backchannelConfig.tailOverlapMode == TailOverlapMode::TreatAsTurn &&
                    playbackRemainingMs >= 0 && static_cast<uint32_t>(playbackRemainingMs) < _backchannelConfig.tailOverlapMs;
                beginOverlap(nearReplyEnd ? OverlapKind::TailTurn : OverlapKind::PendingBargeIn);
                break;
            }
            case transport::AgentState::THINKING:
                transitionTo(transport::AgentState::INTERRUPTED,
                    transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING);
                startInterruptTimeout();
                break;
            case transport::AgentState::LISTENING: {
                cancelFollowUpWindow();
                {
                    std::lock_guard<std::mutex> lock(_backchannelMutex);
                    _finalAfterSpeakingMode = FinalAfterSpeakingMode::None;  // a new turn supersedes it
                }
                // Reported although the state is unchanged: the turn assembler keeps the words of a
                // paused sentence for its next turn, and the LLM drops its speculative request.
                transitionTo(transport::AgentState::LISTENING,
                    transport::AgentStateTransition::USER_STARTED_SPEAKING, 0.0f, true);
                break;
            }
            default:
                // IDLE / INTERRUPTED: ignored - IDLE must be activated by TriggerFrame first
                break;
            }
        }

        void AgentStateProcessor::handleUserTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            _userSpeaking = false;
            if (_currentState.load() == transport::AgentState::SPEAKING) {
                handleOverlapUserTurnEnded();
                return;
            }
            if (_currentState.load() == transport::AgentState::LISTENING) {
                transitionTo(transport::AgentState::THINKING,
                    transport::AgentStateTransition::USER_STOPPED_SPEAKING,
                    frame->confidence);
            }
        }

        void AgentStateProcessor::handleSystemTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            switch (frame->state) {
            case transport::TurnStateFrame::TurnState::START:
                handleSystemTurnStarted(frame);
                break;
            case transport::TurnStateFrame::TurnState::END:
                handleSystemTurnEnded(frame);
                break;
            }
        }

        void AgentStateProcessor::handleSystemTurnStarted(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            const auto state = _currentState.load();
            // From LISTENING too: speech nobody asked for (an agent's greeting, say()) is spoken
            // like a reply, so the user can cut it and its echo isn't taken for a turn.
            if (state == transport::AgentState::THINKING ||
                (state == transport::AgentState::LISTENING && !_userSpeaking)) {
                cancelFollowUpWindow();
                transitionTo(transport::AgentState::SPEAKING,
                    transport::AgentStateTransition::SYNTHESIS_STARTED);
            }
        }

        void AgentStateProcessor::handleSystemTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            switch (_currentState.load()) {
            case transport::AgentState::SPEAKING:
                handleOverlapSystemTurnEnded();
                break;
            case transport::AgentState::INTERRUPTED: {
                cancelInterruptTimeout();
                resolveInterrupt();
                break;
            }
            default:
                break;
            }
        }

        void AgentStateProcessor::handleTranscriptionFrame(
            std::shared_ptr<transport::TranscriptionFrame> frame)
        {
            if (!frame) return;
            if (_currentState.load() == transport::AgentState::SPEAKING) {
                bool overlapActive;
                {
                    std::lock_guard<std::mutex> lock(_backchannelMutex);
                    overlapActive = _overlapKind != OverlapKind::None;
                }
                if (overlapActive) {
                    handleOverlapTranscription(frame);
                    return;
                }
            }
            if (!frame->isFinal) {
                // Always pass partial transcriptions through unchanged
                pushDownstream(frame);
                return;
            }

            auto state = _currentState.load();

            if (state == transport::AgentState::LISTENING) {
                FinalAfterSpeakingMode finalAfterSpeakingMode;
                {
                    std::lock_guard<std::mutex> lock(_backchannelMutex);
                    finalAfterSpeakingMode = _finalAfterSpeakingMode;
                    _finalAfterSpeakingMode = FinalAfterSpeakingMode::None;
                }
                // The reply ended after the user stopped talking over it; this is that speech's final.
                if (finalAfterSpeakingMode == FinalAfterSpeakingMode::ClassifyThenReplay &&
                    classifyBackchannel(frame->transcriptionResult.text, true, _backchannelConfig) == BackchannelVerdict::Backchannel) {
                    spdlog::info("[AgentStateProcessor] Back-channel \"{}\" over the reply's end - not answered",
                        frame->transcriptionResult.text);
                    _metrics.backchannels.increment();
                    if (_backchannelConfig.historyMode == BackchannelHistoryMode::AppendToHistory) {
                        frame->isBackchannel = true;
                        pushDownstream(frame);
                    }
                    return;
                }
                if (finalAfterSpeakingMode != FinalAfterSpeakingMode::None) {
                    replayHeldTurn(frame);
                    return;
                }
            }

            switch (state) {
            case transport::AgentState::LISTENING:
                transitionTo(transport::AgentState::THINKING,
                    transport::AgentStateTransition::TRANSCRIPTION_READY,
                    frame->transcriptionResult.confidence);
                pushDownstream(frame);
                break;
            case transport::AgentState::THINKING:
                // Normal path: TurnStateFrame{END} already moved us to THINKING
                pushDownstream(frame);
                break;
            default:
                // Stale frame from a cancelled turn - drop it
                spdlog::info("[AgentStateProcessor] Dropping stale TranscriptionFrame in state {}",
                    transport::toString(state));
                break;
            }
        }

        void AgentStateProcessor::handleTriggerFrame(
            std::shared_ptr<transport::TriggerFrame> frame)
        {
            if (!frame) return;

            switch (frame->triggerType) {
            case transport::TriggerType::APP_COMMAND:
                handleAppCommandTrigger(frame);
                break;
            case transport::TriggerType::WAKE_WORD:
                handleWakeWordTrigger(frame);
                break;
            case transport::TriggerType::BUTTON_PRESS:
                handleButtonPressTrigger(frame);
                break;
            case transport::TriggerType::SCHEDULED:   // a reminder or app event wants the user's attention
                handleWakeWordTrigger(frame);
                break;
            case transport::TriggerType::PUSH_TO_TALK:
                handlePushToTalkTrigger(frame);
                break;
            default:
                break;
            }
        }

        void AgentStateProcessor::handleAppCommandTrigger(std::shared_ptr<transport::TriggerFrame> frame)
        {
            if (frame->triggerData == "deactivate") {
                transitionTo(transport::AgentState::IDLE,
                    transport::AgentStateTransition::APP_COMMAND);
                return;
            }

            // Any other APP_COMMAND activates the agent
            if (_currentState.load() == transport::AgentState::IDLE) {
                transitionTo(transport::AgentState::LISTENING,
                    transport::AgentStateTransition::APP_COMMAND);
            }
        }

        void AgentStateProcessor::handleWakeWordTrigger(std::shared_ptr<transport::TriggerFrame> frame)
        {
            if (_currentState.load() == transport::AgentState::IDLE) {
                transitionTo(transport::AgentState::LISTENING,
                    transport::AgentStateTransition::WAKE_WORD_DETECTED);
            }
        }

        void AgentStateProcessor::handleButtonPressTrigger(std::shared_ptr<transport::TriggerFrame> frame)
        {
            switch (_currentState.load()) {
            case transport::AgentState::SPEAKING:
                {
                    std::lock_guard<std::mutex> lock(_backchannelMutex);
                    clearOverlapLocked();
                }
                // Manual cancel-speech button - same reaction as a user barging in by
                // voice, so TTSFrameProcessor's existing BARGE_IN_INTERRUPT handling
                // (cancel synthesis, emit SynthesisState::INTERRUPTED) applies unchanged.
                transitionTo(transport::AgentState::INTERRUPTED,
                    transport::AgentStateTransition::BARGE_IN_INTERRUPT);
                startInterruptTimeout();
                break;
            case transport::AgentState::THINKING:
                // Manual cancel-thinking button - same reaction as the user speaking
                // while the LLM is generating, so LLMProcessor's existing
                // USER_SPOKE_WHILE_THINKING handling (transport->cancel(), clear
                // accumulated state) applies unchanged. No synthesis frame will
                // arrive to resolve INTERRUPTED here, so the interrupt timeout
                // fallback in interruptTimeoutLoop() returns the agent to
                // LISTENING/IDLE.
                transitionTo(transport::AgentState::INTERRUPTED,
                    transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING);
                startInterruptTimeout();
                break;
            default:
                break;
            }
        }

        void AgentStateProcessor::handlePushToTalkTrigger(std::shared_ptr<transport::TriggerFrame> frame)
        {
            if (frame->triggerData == "down") {
                _pushToTalkHeld = true;
                cancelFollowUpWindow();
                switch (_currentState.load()) {
                case transport::AgentState::IDLE:
                    transitionTo(transport::AgentState::LISTENING, transport::AgentStateTransition::PUSH_TO_TALK);
                    break;
                case transport::AgentState::SPEAKING:
                case transport::AgentState::THINKING:
                    handleButtonPressTrigger(frame);   // talking over the reply cuts it, like the cancel button
                    break;
                default:
                    break;
                }
                return;
            }
            _pushToTalkHeld = false;
            // Released mid-sentence: the turn ends on its own and gets its answer.
            if (!_alwaysOn && !_userSpeaking && _currentState.load() == transport::AgentState::LISTENING)
                transitionTo(transport::AgentState::IDLE, transport::AgentStateTransition::PUSH_TO_TALK);
        }

        void AgentStateProcessor::handleSynthesisStateFrame(
            std::shared_ptr<transport::SynthesisStateFrame> frame)
        {
            // TurnDetectionProcessor converts SynthesisStateFrame into TurnStateFrame{SYSTEM}
            // downstream, so all state reactions live in handleTurnStateFrame.
            // Nothing to do here - frame is passed upstream by the caller.
        }

        //  State transitions 

        void AgentStateProcessor::transitionTo(
            transport::AgentState next, transport::AgentStateTransition reason,
            float confidence, bool emitWhenUnchanged, int listenWindowMs)
        {
            auto prev = _currentState.exchange(next);
            if (prev == next && !emitWhenUnchanged) return;  // no-op if state unchanged

            if (next != transport::AgentState::SPEAKING) {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                clearOverlapLocked();
                if (next != transport::AgentState::LISTENING) _finalAfterSpeakingMode = FinalAfterSpeakingMode::None;
                if (next == transport::AgentState::IDLE) {
                    _heldFinalTranscription.reset();
                    _replayTurnAfterInterruptResolved = false;
                }
            }

            spdlog::info("[AgentStateProcessor] {} → {} ({}) [conf={:.2f}]",
                transport::toString(prev),
                transport::toString(next),
                transport::toString(reason),
                confidence);

            _metrics.transitions.increment();

            // Capture utterance start when agent enters LISTENING state
            if (next == transport::AgentState::LISTENING) {
                _utteranceStartNs = static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            }

            // Trigger per-utterance report when a complete turn finishes (back to IDLE or LISTENING after SPEAKING)
            if (prev == transport::AgentState::SPEAKING &&
                (next == transport::AgentState::LISTENING || next == transport::AgentState::IDLE)) {
                if (auto obs = _utteranceObserver.lock(); obs && _utteranceStartNs > 0) {
                    obs->onUtteranceEnd(_utteranceStartNs);
                }
            }

            emitAgentStateFrame(next, prev, reason, confidence, listenWindowMs);
        }

        void AgentStateProcessor::emitAgentStateFrame(
            transport::AgentState current,
            transport::AgentState previous,
            transport::AgentStateTransition reason,
            float confidence, int listenWindowMs)
        {
            auto f = std::make_shared<transport::AgentStateFrame>();
            f->listenWindowMs = listenWindowMs;
            f->currentState = current;
            f->previousState = previous;
            f->transition = reason;
            f->triggerConfidence = confidence;
            f->timestamp = transport::Frame::getCurrentTimestamp();
            f->sourceId = getName();
            pushDownstream(f);
        }

        //  Interrupt timeout 

        void AgentStateProcessor::startInterruptTimeout() {
            std::lock_guard<std::mutex> lock(_interruptMutex);
            _interruptPending = true;
            _interruptCv.notify_one();
        }

        void AgentStateProcessor::cancelInterruptTimeout() {
            std::lock_guard<std::mutex> lock(_interruptMutex);
            _interruptPending = false;
            _interruptCv.notify_one();
        }

        void AgentStateProcessor::interruptTimeoutLoop() {
            while (true) {
                std::unique_lock<std::mutex> lock(_interruptMutex);
                _interruptCv.wait(lock, [this] {
                    return _interruptPending || _interruptStop;
                    });

                if (_interruptStop) break;

                // Wait for the timeout while holding the pending flag
                auto timeout = std::chrono::milliseconds(_interruptTimeoutMs);
                bool fired = !_interruptCv.wait_for(lock, timeout, [this] {
                    return !_interruptPending || _interruptStop;
                    });

                if (_interruptStop) break;

                if (fired && _interruptPending) {
                    _interruptPending = false;
                    lock.unlock();
                    spdlog::warn("[AgentStateProcessor] Interrupt timeout - resolving it");
                    resolveInterrupt();
                }
            }
        }

        //  Back-channel handling 

        void AgentStateProcessor::beginOverlap(OverlapKind overlapKind)
        {
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                clearOverlapLocked();
                _overlapKind = overlapKind;
            }
            if (overlapKind == OverlapKind::TailTurn) {
                spdlog::info("[AgentStateProcessor] User spoke over the end of the reply - answering after it");
                return;
            }
            spdlog::info("[AgentStateProcessor] User spoke over the agent - barge-in pending ({})",
                _backchannelConfig.pendingPlaybackMode == transport::PendingPlaybackMode::Duck ? "duck"
                : _backchannelConfig.pendingPlaybackMode == transport::PendingPlaybackMode::Pause ? "pause" : "continue");
            transitionTo(transport::AgentState::SPEAKING, transport::AgentStateTransition::BARGE_IN_PENDING,
                0.0f, true);
        }

        void AgentStateProcessor::handleOverlapAudio(const transport::AudioFrame& audioFrame)
        {
            if (audioFrame.sampleRate == 0) return;
            const uint32_t durationMs = static_cast<uint32_t>(
                static_cast<uint64_t>(audioFrame.numSamples) * 1000 / audioFrame.sampleRate);
            bool commitNow = false, decideNow = false;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_overlapKind != OverlapKind::PendingBargeIn) return;
                if (!_userSpeechEnded) {
                    _pendingSpeechDurationMs += durationMs;
                    commitNow = _pendingSpeechDurationMs > _backchannelConfig.commitAfterPendingMs;
                } else {
                    _audioDurationSinceSpeechEndMs += durationMs;
                    decideNow = _audioDurationSinceSpeechEndMs > _backchannelConfig.finalTranscriptionWaitMs;
                }
            }
            if (commitNow) commitBargeIn(false, "speech too long for a back-channel");
            else if (decideNow) decideOverlap();
        }

        void AgentStateProcessor::handleOverlapUserTurnEnded()
        {
            std::lock_guard<std::mutex> lock(_backchannelMutex);
            if (_overlapKind == OverlapKind::None) return;
            _userSpeechEnded = true;
            _audioDurationSinceSpeechEndMs = 0;
        }

        void AgentStateProcessor::handleOverlapTranscription(std::shared_ptr<transport::TranscriptionFrame> frame)
        {
            const std::string& text = frame->transcriptionResult.text;
            OverlapKind overlapKind;
            bool userSpeechEnded;
            std::string overlapText;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                overlapKind = _overlapKind;
                if (frame->isFinal) {
                    if (!text.empty()) {
                        if (!_overlapFinalText.empty()) _overlapFinalText += ' ';
                        _overlapFinalText += text;
                    }
                    _overlapFinalConfidence = std::max(_overlapFinalConfidence, frame->transcriptionResult.confidence);
                    _latestPartialTranscription.clear();
                } else if (!text.empty()) {
                    _latestPartialTranscription = text;
                }
                userSpeechEnded = _userSpeechEnded;
                overlapText = overlapTextLocked();
            }
            // Partials still reach the LLM's fallback cache; finals are held until the overlap is decided.
            if (!frame->isFinal) pushDownstream(frame);
            if (overlapKind != OverlapKind::PendingBargeIn) return;

            if (frame->isFinal && userSpeechEnded) {
                decideOverlap();
                return;
            }
            if (classifyBackchannel(overlapText, false, _backchannelConfig) == BackchannelVerdict::Interrupt)
                commitBargeIn(userSpeechEnded, "words");
        }

        void AgentStateProcessor::decideOverlap()
        {
            bool userSpeechEnded;
            std::string overlapText;
            float confidence;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_overlapKind != OverlapKind::PendingBargeIn) return;
                userSpeechEnded = _userSpeechEnded;
                overlapText = overlapTextLocked();
                // Deciding on a partial (no final arrived): no STT confidence to go on.
                confidence = _overlapFinalText.empty() ? 1.0f : _overlapFinalConfidence;
            }
            if (classifyBackchannel(overlapText, true, _backchannelConfig, confidence) == BackchannelVerdict::Backchannel)
                dismissBackchannel();
            else
                commitBargeIn(userSpeechEnded, "words after the user stopped");
        }

        void AgentStateProcessor::commitBargeIn(bool userSpeechEnded, const char* reason)
        {
            std::shared_ptr<transport::TranscriptionFrame> forwardNow;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_overlapKind != OverlapKind::PendingBargeIn) return;
                if (userSpeechEnded) {
                    // The user's turn already ended unseen: replay it once the interrupt resolves.
                    _heldFinalTranscription = takeOverlapTranscriptionLocked(true);
                    _replayTurnAfterInterruptResolved = _heldFinalTranscription != nullptr;
                } else {
                    // Still talking: the turn ends normally later; finals heard so far go on now.
                    forwardNow = takeOverlapTranscriptionLocked(false);
                }
                clearOverlapLocked();
            }
            spdlog::info("[AgentStateProcessor] Barge-in committed ({}{})", reason,
                userSpeechEnded ? ", replaying the turn" : "");
            _metrics.bargeInsCommitted.increment();
            transitionTo(transport::AgentState::INTERRUPTED, transport::AgentStateTransition::BARGE_IN_INTERRUPT);
            startInterruptTimeout();
            if (forwardNow) pushDownstream(forwardNow);
        }

        void AgentStateProcessor::dismissBackchannel()
        {
            std::shared_ptr<transport::TranscriptionFrame> recorded;
            std::string overlapText;
            float confidence;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_overlapKind != OverlapKind::PendingBargeIn) return;
                overlapText = overlapTextLocked();
                confidence = _overlapFinalConfidence;
                if (_backchannelConfig.historyMode == BackchannelHistoryMode::AppendToHistory)
                    recorded = takeOverlapTranscriptionLocked(true);
                clearOverlapLocked();
            }
            spdlog::info("[AgentStateProcessor] Back-channel \"{}\" (conf={:.2f}) - agent keeps speaking",
                overlapText, confidence);
            _metrics.backchannels.increment();
            transitionTo(transport::AgentState::SPEAKING, transport::AgentStateTransition::BACKCHANNEL, 0.0f, true);
            if (recorded) {
                recorded->isBackchannel = true;
                pushDownstream(recorded);
            }
        }

        void AgentStateProcessor::handleOverlapSystemTurnEnded()
        {
            std::shared_ptr<transport::TranscriptionFrame> replay;
            std::shared_ptr<transport::TranscriptionFrame> forwardNow;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_overlapKind != OverlapKind::None) {
                    if (!_userSpeechEnded) {
                        // Still talking as the reply ends: it becomes an ordinary user turn.
                        forwardNow = takeOverlapTranscriptionLocked(false);
                    } else if (_overlapFinalText.empty()) {
                        _finalAfterSpeakingMode = _overlapKind == OverlapKind::TailTurn
                            ? FinalAfterSpeakingMode::ReplayAsTurn : FinalAfterSpeakingMode::ClassifyThenReplay;
                    } else if (_overlapKind == OverlapKind::TailTurn ||
                               classifyBackchannel(overlapTextLocked(), true, _backchannelConfig) != BackchannelVerdict::Backchannel) {
                        replay = takeOverlapTranscriptionLocked(true);
                    }
                    clearOverlapLocked();
                }
            }
            finishReply(transport::AgentStateTransition::SYNTHESIS_COMPLETED);
            if (forwardNow) pushDownstream(forwardNow);
            if (replay && _currentState.load() == transport::AgentState::LISTENING) replayHeldTurn(replay);
        }

        void AgentStateProcessor::finishReply(transport::AgentStateTransition reason)
        {
            // Push-to-talk held through the reply: the user is answering now.
            const int windowMs = _pushToTalkHeld ? -1 : _followUpListenMs.load();
            if (windowMs == 0) {
                transitionTo(_alwaysOn ? transport::AgentState::LISTENING : transport::AgentState::IDLE, reason);
                return;
            }
            transitionTo(transport::AgentState::LISTENING, reason, 0.0f, false, windowMs);
            {
                std::lock_guard<std::mutex> lock(_followUpMutex);
                _followUpOpen = true;
            }
            if (windowMs > 0) armFollowUpWindow(windowMs);
        }

        void AgentStateProcessor::armFollowUpWindow(int ms)
        {
            {
                std::lock_guard<std::mutex> lock(_followUpMutex);
                _followUpArmed = true;
                _followUpDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            }
            _followUpCv.notify_one();
        }

        void AgentStateProcessor::cancelFollowUpWindow()
        {
            {
                std::lock_guard<std::mutex> lock(_followUpMutex);
                _followUpOpen = _followUpArmed = false;
            }
            _followUpCv.notify_one();
        }

        void AgentStateProcessor::followUpLoop()
        {
            std::unique_lock<std::mutex> lock(_followUpMutex);
            while (true) {
                _followUpCv.wait(lock, [this] { return _followUpArmed || _followUpStop; });
                if (_followUpStop) return;
                const auto deadline = _followUpDeadline;
                if (_followUpCv.wait_until(lock, deadline, [this, deadline] {
                        return _followUpStop || !_followUpArmed || _followUpDeadline != deadline; }))
                    continue;   // stopped, cancelled, or re-armed by a newer reply
                _followUpArmed = _followUpOpen = false;
                lock.unlock();
                // Nobody spoke: back to waiting for the wake word or button.
                if (_currentState.load() == transport::AgentState::LISTENING && !_userSpeaking) {
                    transitionTo(_alwaysOn ? transport::AgentState::LISTENING : transport::AgentState::IDLE,
                                 transport::AgentStateTransition::FOLLOW_UP_WINDOW_CLOSED, 0.0f, true);
                }
                lock.lock();
            }
        }

        void AgentStateProcessor::resolveInterrupt()
        {
            finishReply(transport::AgentStateTransition::INTERRUPT_RESOLVED);
            const auto next = _currentState.load();
            std::shared_ptr<transport::TranscriptionFrame> replay;
            {
                std::lock_guard<std::mutex> lock(_backchannelMutex);
                if (_replayTurnAfterInterruptResolved) replay = std::move(_heldFinalTranscription);
                _heldFinalTranscription.reset();
                _replayTurnAfterInterruptResolved = false;
            }
            if (replay && next == transport::AgentState::LISTENING) replayHeldTurn(replay);
        }

        void AgentStateProcessor::replayHeldTurn(std::shared_ptr<transport::TranscriptionFrame> heldTranscription)
        {
            spdlog::info("[AgentStateProcessor] Replaying the user's turn spoken over the agent: \"{}\"",
                heldTranscription->transcriptionResult.text);
            transitionTo(transport::AgentState::THINKING, transport::AgentStateTransition::USER_STOPPED_SPEAKING,
                heldTranscription->transcriptionResult.confidence);
            pushDownstream(heldTranscription);
        }

        void AgentStateProcessor::clearOverlapLocked()
        {
            _overlapKind = OverlapKind::None;
            _userSpeechEnded = false;
            _pendingSpeechDurationMs = 0;
            _audioDurationSinceSpeechEndMs = 0;
            _overlapFinalText.clear();
            _overlapFinalConfidence = 0.0f;
            _latestPartialTranscription.clear();
        }

        std::string AgentStateProcessor::overlapTextLocked() const
        {
            if (_latestPartialTranscription.empty()) return _overlapFinalText;
            if (_overlapFinalText.empty()) return _latestPartialTranscription;
            return _overlapFinalText + " " + _latestPartialTranscription;
        }

        std::shared_ptr<transport::TranscriptionFrame> AgentStateProcessor::takeOverlapTranscriptionLocked(bool includePartial)
        {
            const std::string text = includePartial ? overlapTextLocked() : _overlapFinalText;
            if (text.empty()) return nullptr;
            auto frame = std::make_shared<transport::TranscriptionFrame>();
            frame->isFinal = true;
            frame->transcriptionResult.text = text;
            frame->transcriptionResult.confidence = _overlapFinalConfidence;
            frame->timestamp = transport::Frame::getCurrentTimestamp();
            frame->sourceId = getName();
            return frame;
        }

    } // namespace pipeline
} // namespace va
