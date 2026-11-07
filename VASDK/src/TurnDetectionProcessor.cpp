#include "VASDK/TurnDetectionProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include "TurnDetectionProcessor.h"

namespace va {
    namespace pipeline {
        TurnDetectionProcessor::TurnDetectionProcessor(std::shared_ptr<audio::TurnDetectionProvider> provider) : _provider(provider)
        {
        }

        void TurnDetectionProcessor::setMetricsConfig(const va::metrics::TurnDetectionMetricsConfig& config)
        {
            _metrics.config = config;
            FrameProcessor::setMetricsConfig(static_cast<const va::metrics::ProcessorMetricsConfig&>(config));
        }

        bool TurnDetectionProcessor::initialize()
        {
            if (!FrameProcessor::initialize()) return false;

            if (_metrics.config.enabled && _collector) {
                va::metrics::MetricDescriptor infDesc{
                    "turn_inference_ms",
                    va::metrics::MetricType::Sampler,
                    "TurnDetection provider evaluate() latency",
                    _metrics.config.inferenceWindow};
                _metrics.inference.initialize(_collector, infDesc);

                if (_metrics.config.inferenceAlertMs > 0.0) {
                    va::metrics::MetricDescriptor gDesc{
                        "turn_inference_alert", va::metrics::MetricType::Gauge,
                        "TurnDetection latency alert"};
                    _metrics.inferenceAlert.initialize(_collector, gDesc);
                    _metrics.inferenceAlert.setThreshold(_metrics.config.inferenceAlertMs,
                                                          _collector->alertDispatcher());
                }
            }
            return true;
        }

        bool TurnDetectionProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const
        {
            if (frame == nullptr) {
                return false;
            }
            switch (frame->getType())
            {
            case va::transport::FrameType::TURN_STATE:
            case va::transport::FrameType::VAD_STATE:
            case va::transport::FrameType::SYNTHESIS_STATE:
            case va::transport::FrameType::CONTROL:   // TurnPolicyFrame
            return true;
            case va::transport::FrameType::AUDIO:
            return _provider != nullptr;
            default:
            return false;
            }
        }

        void TurnDetectionProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction)
        {
            VASDK_CTRACK_NAME("TurnDetect::processFrame");
            spdlog::debug("[{}] Processing {} frame: type={}, id={}, seq={}, timestamp={}",
                direction == FlowDirection::DOWNSTREAM ? "DOWNSTREAM" : "UPSTREAM",
                getName(),
                va::transport::toString(frame->getType()),
                frame->frameId,
                frame->sequenceId,
                frame->timestamp);

            // How to listen for the next answer: patient, a model's guess during a pause doesn't end it.
            if (frame->getType() == transport::FrameType::CONTROL) {
                if (auto policy = std::dynamic_pointer_cast<transport::TurnPolicyFrame>(frame))
                    _patient = policy->turnEnd == transport::TurnPolicyFrame::TurnEnd::Patient;
                if (direction == FlowDirection::UPSTREAM) pushUpstream(frame);
                else pushDownstream(frame);
                return;
            }

            // SynthesisStateFrame flows upstream - update _agentIsSpeaking and pass through.
            if (direction == FlowDirection::UPSTREAM &&
                frame->getType() == transport::FrameType::SYNTHESIS_STATE) {
                auto synthFrame = std::dynamic_pointer_cast<transport::SynthesisStateFrame>(frame);
                processSynthesisStateFrame(synthFrame, direction);
                return;
            }

            const auto vadFrame = frame->getType() == transport::FrameType::VAD_STATE
                ? std::static_pointer_cast<transport::VADStateFrame>(frame) : nullptr;
            if (vadFrame) _vadState = vadFrame->state;

            // If a provider is set, delegate to it. Otherwise apply default VAD → turn mapping via TURN_STATE and VAD_STATE frames.
            if (_provider != nullptr) {
                processWithProvider(frame, direction);
            }
            else {
                switch (frame->getType()) {
                case transport::FrameType::TURN_STATE:
                    if (auto turnFrame = std::dynamic_pointer_cast<transport::TurnStateFrame>(frame)) {
                        processTurnStateFrame(turnFrame, direction);
                    }
                    break;
                case transport::FrameType::VAD_STATE:
                    if (auto vadFrame = std::dynamic_pointer_cast<transport::VADStateFrame>(frame)) {
                        processVadStateFrame(vadFrame, direction);
                    }
                    break;
                default:
                    break;
                }
            }

            if (vadFrame && _heldUserStart) {
                if (vadFrame->state == transport::VADState::ONGOING) {
                    // The agent may have started its reply while the speech was being confirmed: then it interrupts it.
                    _heldUserStart->type = _agentIsSpeaking ? transport::TurnStateFrame::TurnType::INTERRUPT
                                                            : transport::TurnStateFrame::TurnType::NORMAL;
                    pushDownstream(_heldUserStart);
                    spdlog::info("[{}] Speech confirmed - emitted held user TURN_STATE START (type={})",
                        getName(), static_cast<int>(_heldUserStart->type));
                    _heldUserStart.reset();
                } else if (vadFrame->state == transport::VADState::QUIET) {
                    spdlog::info("[{}] VAD blip, not speech - dropped user turn start", getName());
                    _heldUserStart.reset();
                }
            }

            // Push the original frame along by default; providers may suppress or modify this.
            switch (direction) {
            case FlowDirection::DOWNSTREAM: {
                pushDownstream(frame);
            } break;
            case FlowDirection::UPSTREAM: {
                pushUpstream(frame);
            } break;
            }
        }

        void TurnDetectionProcessor::processTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame, FlowDirection direction)
        {
            // No special handling needed; TURN_STATE frames pass through unchanged via processFrame().
            (void)frame; (void)direction;
        }

        void TurnDetectionProcessor::processVadStateFrame(std::shared_ptr<transport::VADStateFrame> frame, FlowDirection direction)
        {
            switch (frame->state) {
            case transport::VADState::STARTING: {
                auto type = _agentIsSpeaking
                    ? transport::TurnStateFrame::TurnType::INTERRUPT
                    : transport::TurnStateFrame::TurnType::NORMAL;
                emitTurnFrame(transport::TurnStateFrame::TurnActor::USER,
                    transport::TurnStateFrame::TurnState::START,
                    type,
                    frame->confidence);
                break;
            }
            case transport::VADState::ENDED:
                emitTurnFrame(transport::TurnStateFrame::TurnActor::USER,
                    transport::TurnStateFrame::TurnState::END,
                    transport::TurnStateFrame::TurnType::NORMAL,
                    frame->confidence);
                break;
            default:
                break;
            }
        }

        void TurnDetectionProcessor::processWithProvider(std::shared_ptr<transport::Frame> frame, FlowDirection direction)
        {
            if (frame->getType() == transport::FrameType::VAD_STATE) {
                auto state = std::static_pointer_cast<transport::VADStateFrame>(frame)->state;
                if (state == transport::VADState::STARTING) _userSpeechOpen = true, _pauseEndHeld = false;
                else if (state == transport::VADState::ENDED || state == transport::VADState::QUIET) _userSpeechOpen = false;
            }

            audio::TurnDetectionProvider::TurnEvaluation evaluation;
            {
                va::metrics::ScopedTimer t(_metrics.inference.get());
                evaluation = _provider->evaluate(frame, isAgentSpeaking());
            }
            // Patient, or a pause's turn end held back: the speech has ended and the model still waits
            // (or already spoke); the silence is the end.
            if ((_patient || _pauseEndHeld) && evaluation.decision == audio::TurnDetectionProvider::Decision::NONE &&
                frame->getType() == transport::FrameType::VAD_STATE &&
                std::static_pointer_cast<transport::VADStateFrame>(frame)->state == transport::VADState::ENDED) {
                evaluation.decision = audio::TurnDetectionProvider::Decision::TURN_END;
                evaluation.actor = transport::TurnStateFrame::TurnActor::USER;
                evaluation.confidence = 1.0f;
                _pauseEndHeld = false;
            }
            switch (evaluation.decision) {
            case audio::TurnDetectionProvider::Decision::NONE:
            // No turn event detected, do nothing
            break;
            case audio::TurnDetectionProvider::Decision::TURN_START: {
                emitTurnFrame(evaluation.actor, transport::TurnStateFrame::TurnState::START, evaluation.type, evaluation.confidence);
            } break;
            case audio::TurnDetectionProvider::Decision::TURN_END: {
                if (_patient && evaluation.actor == transport::TurnStateFrame::TurnActor::USER && _userSpeechOpen) {
                    spdlog::debug("[{}] Patient turn: a pause isn't the end", getName());
                    break;   // the VAD's end decides
                }
                // Over the agent the model judges a fragment heard through its voice ("yeah", before
                // "wait, stop"); ended there, it's dismissed as a back-channel. The VAD's end ends it.
                if (evaluation.actor == transport::TurnStateFrame::TurnActor::USER && _userSpeechOpen && _agentIsSpeaking) {
                    spdlog::debug("[{}] Over the agent: a pause isn't the end", getName());
                    _pauseEndHeld = true;
                    break;
                }
                emitTurnFrame(evaluation.actor, transport::TurnStateFrame::TurnState::END, evaluation.type, evaluation.confidence);
                if (evaluation.actor == transport::TurnStateFrame::TurnActor::USER && _userSpeechOpen) {
                    // The provider called it during a pause: have the VAD close the segment now.
                    _userSpeechOpen = false;
                    auto end = std::make_shared<transport::TurnStateFrame>(evaluation.actor,
                        transport::TurnStateFrame::TurnState::END, evaluation.type, evaluation.confidence);
                    end->sourceId = getName();
                    pushUpstream(end);
                }
            } break;
            }
        }

        void TurnDetectionProcessor::processSynthesisStateFrame(std::shared_ptr<transport::SynthesisStateFrame> frame, FlowDirection direction)
        {
            switch (frame->state) {
            case transport::SynthesisState::STARTED:
            _agentIsSpeaking = true;
            spdlog::debug("[{}] agentIsSpeaking = true (SynthesisStateFrame::STARTED)", getName());
            emitTurnFrame(transport::TurnStateFrame::TurnActor::SYSTEM,
                transport::TurnStateFrame::TurnState::START,
                transport::TurnStateFrame::TurnType::NORMAL,
                1.0f);
            break;
            case transport::SynthesisState::COMPLETED:
            case transport::SynthesisState::INTERRUPTED:
            case transport::SynthesisState::ERROR:
            _agentIsSpeaking = false;
            spdlog::debug("[{}] agentIsSpeaking = false (SynthesisStateFrame::{})",
                getName(), transport::toString(frame->state));
            emitTurnFrame(transport::TurnStateFrame::TurnActor::SYSTEM,
                transport::TurnStateFrame::TurnState::END,
                transport::TurnStateFrame::TurnType::NORMAL,
                1.0f);
            break;
            }
            pushUpstream(frame);
        }

        void TurnDetectionProcessor::emitTurnFrame(transport::TurnStateFrame::TurnActor actor,
            transport::TurnStateFrame::TurnState state,
            transport::TurnStateFrame::TurnType type,
            float confidence)
        {
            auto turnFrame = std::make_shared<transport::TurnStateFrame>(actor, state, type, confidence);
            turnFrame->sourceId = getName();
            if (actor == transport::TurnStateFrame::TurnActor::USER &&
                state == transport::TurnStateFrame::TurnState::START &&
                _vadState == transport::VADState::STARTING) {
                _heldUserStart = turnFrame;  // sent when the VAD confirms speech
                return;
            }
            pushDownstream(turnFrame);
            spdlog::info("[{}] Emitted TURN_STATE frame: actor={}, state={}, type={}, confidence={:.1f}%",
                getName(),
                static_cast<int>(actor),
                static_cast<int>(state),
                static_cast<int>(type),
                confidence * 100.0f);
        }
    }
}
