#pragma once
#include <functional>
#include <unordered_map>
#include <atomic>
#include "FrameProcessor.h"
#include "TurnDetectionProvider.h"
#include "metrics/MetricHandles.h"

namespace va {
    namespace pipeline {

        /**
         * @brief Turn detection processor - converts VAD speech boundaries into
         *        conversation-level turn events (TURN_STATE frames).
         *
         * By default the processor uses a simple threshold rule:
         *   VAD STARTING  →  USER_START
         *   VAD ENDED     →  USER_END
         *
         * A smarter provider can be injected via setProvider() to apply
         * confidence thresholds, rolling averages, and INTERRUPT classification.
         * See VASDK_3P_IMPL/SmartTurnDetection for the reference implementation.
         *
         * See docs/reference/processors.md for placement in the standard chain.
         */
        class VASDK_EXPORT TurnDetectionProcessor : public FrameProcessor {
            public:
            TurnDetectionProcessor(std::shared_ptr<audio::TurnDetectionProvider> provider = nullptr);

            //  Agent speaking flag 

            /**
             * @brief Notify the processor whether the agent's TTS is active.
             *
             * Wire to TTSFrameProcessor callbacks in main_pipeline.cpp:
             *   tts->onSynthesisStarted   = [td]{ td->setAgentSpeaking(true);  };
             *   tts->onSynthesisCompleted = [td]{ td->setAgentSpeaking(false); };
             *
             * Used by providers to classify turns as INTERRUPT vs NORMAL.
             */
            void setAgentSpeaking(bool speaking) { _agentIsSpeaking = speaking; }
            bool isAgentSpeaking() const { return _agentIsSpeaking; }

            void setMetricsConfig(const va::metrics::TurnDetectionMetricsConfig& config);
            const va::metrics::TurnDetectionMetricsConfig& getTurnDetectionMetricsConfig() const { return _metrics.config; }

            //  FrameProcessor interface 

            std::string getName() const override { return "TurnDetectionProcessor"; }
            bool initialize() override;
            virtual bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
            virtual void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

            private:
            void processTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame, FlowDirection direction);
            void processVadStateFrame(std::shared_ptr<transport::VADStateFrame> frame, FlowDirection direction);
            void processWithProvider(std::shared_ptr<transport::Frame> frame, FlowDirection direction);
            void processSynthesisStateFrame(std::shared_ptr<transport::SynthesisStateFrame> frame, FlowDirection direction);

            /** Emit a TURN_STATE frame downstream. */
            void emitTurnFrame(
                transport::TurnStateFrame::TurnActor actor,
                transport::TurnStateFrame::TurnState state,
                transport::TurnStateFrame::TurnType  type,
                float confidence);

            std::shared_ptr<audio::TurnDetectionProvider> _provider;
            std::atomic<bool> _agentIsSpeaking{ false };
            struct Metrics {
                va::metrics::TurnDetectionMetricsConfig config;
                va::metrics::SamplerHandle              inference;  // provider evaluate() latency ms
                va::metrics::GaugeHandle                inferenceAlert;
            } _metrics;
        };

    } // namespace pipeline
} // namespace va