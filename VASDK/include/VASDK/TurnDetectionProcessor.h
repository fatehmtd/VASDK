#pragma once
#include <functional>
#include <unordered_map>
#include <atomic>
#include <optional>
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

            /**
             * @brief How long a pause must last before a provider's turn end during it counts (default
             *        0: at once). A shorter pause is often a breath mid-sentence: the end is held until
             *        the pause has lasted this long, and dropped if the user speaks again first (the
             *        VAD's end then ends the turn). Whatever the provider. Before start().
             */
            void setMinPauseMs(uint32_t ms) { _minPauseMs = ms; }
            uint32_t minPauseMs() const { return _minPauseMs; }

            void setMetricsConfig(const va::metrics::TurnDetectionMetricsConfig& config);
            const va::metrics::TurnDetectionMetricsConfig& getTurnDetectionMetricsConfig() const { return _metrics.config; }

            //  FrameProcessor interface 

            std::string getName() const override { return "TurnDetectionProcessor"; }
            std::string getProviderName() const override { return _provider ? _provider->getName() : std::string(); }
            bool initialize() override;
            virtual bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
            virtual void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

            private:
            void processTurnStateFrame(std::shared_ptr<transport::TurnStateFrame> frame, FlowDirection direction);
            void processVadStateFrame(std::shared_ptr<transport::VADStateFrame> frame, FlowDirection direction);
            void processWithProvider(std::shared_ptr<transport::Frame> frame, FlowDirection direction);
            void processSynthesisStateFrame(std::shared_ptr<transport::SynthesisStateFrame> frame, FlowDirection direction);
            // The provider called the user's turn over during a pause: end it, and have the VAD close the segment.
            void endUserTurnAtPause(const audio::TurnDetectionProvider::TurnEvaluation& evaluation);
            // Pause bookkeeping for setMinPauseMs: true when a held end is due (the pause has lasted).
            bool trackPause(const transport::Frame& frame);

            /** Emit a TURN_STATE frame downstream. */
            void emitTurnFrame(
                transport::TurnStateFrame::TurnActor actor,
                transport::TurnStateFrame::TurnState state,
                transport::TurnStateFrame::TurnType  type,
                float confidence);

            std::shared_ptr<audio::TurnDetectionProvider> _provider;
            std::atomic<bool> _agentIsSpeaking{ false };
            bool _userSpeechOpen{ false };  // VAD between STARTING and ENDED
            bool _patient{ false };         // the question's answer has pauses (TurnPolicyFrame): only the VAD's end ends it
            // A turn end at a pause held back because the agent was speaking: the VAD's end ends the turn
            // (the provider, having called it, won't again).
            bool _pauseEndHeld{ false };
            uint32_t _minPauseMs{ 0 };
            uint64_t _pauseSamples{ 0 };       // audio heard since the current pause began
            uint32_t _pauseSampleRate{ 16000 };
            bool     _inPause{ false };        // VAD PAUSED or ENDING
            // A turn end the provider called before the pause lasted _minPauseMs: sent when it has, dropped if
            // the user goes on.
            std::optional<audio::TurnDetectionProvider::TurnEvaluation> _heldPauseEnd;
            // A user turn START is held while the VAD is only STARTING and sent once speech is
            // confirmed (ONGOING): a noise blip (STARTING -> QUIET) must not barge in on the agent.
            transport::VADState _vadState{ transport::VADState::QUIET };
            std::shared_ptr<transport::TurnStateFrame> _heldUserStart;
            struct Metrics {
                va::metrics::TurnDetectionMetricsConfig config;
                va::metrics::SamplerHandle              inference;  // provider evaluate() latency ms
                va::metrics::GaugeHandle                inferenceAlert;
            } _metrics;
        };

    } // namespace pipeline
} // namespace va