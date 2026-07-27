#include "VASDK/AgentStateProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include "AgentStateProcessor.h"

namespace va {
    namespace pipeline {

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
            switch (_currentState.load()) {
            case transport::AgentState::SPEAKING:
                if (frame->type == transport::TurnStateFrame::TurnType::INTERRUPT) {
                    transitionTo(transport::AgentState::INTERRUPTED,
                        transport::AgentStateTransition::BARGE_IN_INTERRUPT);
                    startInterruptTimeout();
                }
                break;
            case transport::AgentState::THINKING:
                transitionTo(transport::AgentState::INTERRUPTED,
                    transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING);
                startInterruptTimeout();
                break;
            case transport::AgentState::LISTENING:
                transitionTo(transport::AgentState::LISTENING,
                    transport::AgentStateTransition::USER_STARTED_SPEAKING);
                break;
            default:
                // IDLE / INTERRUPTED: ignored - IDLE must be activated by TriggerFrame first
                break;
            }
        }

        void AgentStateProcessor::handleUserTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame)
        {
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
            if (_currentState.load() == transport::AgentState::THINKING) {
                transitionTo(transport::AgentState::SPEAKING,
                    transport::AgentStateTransition::SYNTHESIS_STARTED);
            }
        }

        void AgentStateProcessor::handleSystemTurnEnded(std::shared_ptr<transport::TurnStateFrame> frame)
        {
            switch (_currentState.load()) {
            case transport::AgentState::SPEAKING: {
                auto next = _alwaysOn ? transport::AgentState::LISTENING
                                      : transport::AgentState::IDLE;
                transitionTo(next, transport::AgentStateTransition::SYNTHESIS_COMPLETED);
                break;
            }
            case transport::AgentState::INTERRUPTED: {
                cancelInterruptTimeout();
                auto next = _alwaysOn ? transport::AgentState::LISTENING
                                      : transport::AgentState::IDLE;
                transitionTo(next, transport::AgentStateTransition::INTERRUPT_RESOLVED);
                break;
            }
            default:
                break;
            }
        }

        void AgentStateProcessor::handleTranscriptionFrame(
            std::shared_ptr<transport::TranscriptionFrame> frame)
        {
            if (!frame || !frame->isFinal) {
                // Always pass partial transcriptions through unchanged
                pushDownstream(frame);
                return;
            }

            auto state = _currentState.load();

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
            float confidence)
        {
            auto prev = _currentState.exchange(next);
            if (prev == next) return;  // no-op if state unchanged

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

            emitAgentStateFrame(next, prev, reason, confidence);
        }

        void AgentStateProcessor::emitAgentStateFrame(
            transport::AgentState current,
            transport::AgentState previous,
            transport::AgentStateTransition reason,
            float confidence)
        {
            auto f = std::make_shared<transport::AgentStateFrame>();
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
                    auto next = _alwaysOn ? transport::AgentState::LISTENING
                                          : transport::AgentState::IDLE;
                    spdlog::warn("[AgentStateProcessor] Interrupt timeout - forcing {}",
                        transport::toString(next));
                    transitionTo(next, transport::AgentStateTransition::INTERRUPT_RESOLVED);
                }
            }
        }

    } // namespace pipeline
} // namespace va
