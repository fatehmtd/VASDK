#pragma once

#include "FrameProcessor.h"
#include "Frame.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include "metrics/UtteranceObserver.h"
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace va {
    namespace pipeline {

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
         * Wake-word mode: remains IDLE until TriggerFrame{WAKE_WORD/APP_COMMAND} arrives.
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
             * @brief Timeout before forcing INTERRUPTED → LISTENING if TTS does not
             *        emit SynthesisStateFrame{INTERRUPTED}.  Default: 500ms.
             */
            void setInterruptTimeoutMs(uint32_t ms) { _interruptTimeoutMs = ms; }

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
                              float confidence = 0.0f);
            void emitAgentStateFrame(transport::AgentState current,
                                     transport::AgentState previous,
                                     transport::AgentStateTransition reason,
                                     float confidence = 0.0f);

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
            void handleSynthesisStateFrame(std::shared_ptr<transport::SynthesisStateFrame> frame);

            void startInterruptTimeout();
            void cancelInterruptTimeout();
            void interruptTimeoutLoop();

            std::atomic<transport::AgentState> _currentState{ transport::AgentState::IDLE };
            bool _alwaysOn{ true };
            uint32_t _interruptTimeoutMs{ 500 };

            // Interrupt timeout fallback thread
            std::thread             _interruptTimeoutThread;
            std::mutex              _interruptMutex;
            std::condition_variable _interruptCv;
            bool                    _interruptPending{ false };
            bool                    _interruptStop{ false };

            // Metrics
            std::weak_ptr<va::metrics::UtteranceObserverInterface> _utteranceObserver;
            struct Metrics {
                va::metrics::CounterHandle transitions;
            } _metrics;
            uint64_t _utteranceStartNs{0};
        };

    } // namespace pipeline
} // namespace va
