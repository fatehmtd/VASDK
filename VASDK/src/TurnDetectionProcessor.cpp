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

            // SynthesisStateFrame flows upstream - update _agentIsSpeaking and pass through.
            if (direction == FlowDirection::UPSTREAM &&
                frame->getType() == transport::FrameType::SYNTHESIS_STATE) {
                auto synthFrame = std::dynamic_pointer_cast<transport::SynthesisStateFrame>(frame);
                processSynthesisStateFrame(synthFrame, direction);
                return;
            }

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
            audio::TurnDetectionProvider::TurnEvaluation evaluation;
            {
                va::metrics::ScopedTimer t(_metrics.inference.get());
                evaluation = _provider->evaluate(frame, isAgentSpeaking());
            }
            switch (evaluation.decision) {
            case audio::TurnDetectionProvider::Decision::NONE:
            // No turn event detected, do nothing
            break;
            case audio::TurnDetectionProvider::Decision::TURN_START: {
                emitTurnFrame(evaluation.actor, transport::TurnStateFrame::TurnState::START, evaluation.type, evaluation.confidence);
            } break;
            case audio::TurnDetectionProvider::Decision::TURN_END: {
                emitTurnFrame(evaluation.actor, transport::TurnStateFrame::TurnState::END, evaluation.type, evaluation.confidence);
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
