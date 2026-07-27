#pragma once

#include "FrameProcessor.h"
#include "VADProvider.h"
#include "AudioRingBuffer.h"
#include "metrics/MetricHandles.h"
#include <vector>

namespace va {
    namespace pipeline {

        /**
         * @brief Configuration for VAD timing thresholds
         */
        struct VADConfig {
            uint64_t vadThresholdMs = 200;          // Time window for considering voice as detected
            uint64_t minSpeechDurationMs = 250;     // Minimum speech duration to confirm speech presence
            uint64_t maxSpeechPauseDurationMs = 500; // Max pause before considering speech ended
            uint64_t maxSpeechEndDurationMs = 1000;  // Max time in ending state before fully ended
            int resumingSilenceFramesToPause = 3;    // Consecutive silent frames in RESUMING before going back to PAUSED
        };

        /**
         * @brief VAD processor - detects voice activity and generates VADStateFrames
         *
         * Implements a sophisticated state machine with the following states:
         * - QUIET: No speech detected
         * - STARTING: Voice detected, waiting to confirm speech presence
         * - ONGOING: Confirmed speech in progress
         * - PAUSED: Speech paused (hesitation)
         * - RESUMING: Speech resuming after pause
         * - ENDING: Speech ending, waiting to confirm end
         * - ENDED: Speech has ended, will transition back to QUIET
         */
        class VASDK_EXPORT VADFrameProcessor : public FrameProcessor {
            public:
            /**
             * @brief Constructor for VADFrameProcessor
             * @param vadProvider Shared VAD provider for voice activity detection
             * @param lookbackMultiplier Lookback window as a multiple of frame size, to include past audio context (default 4.0 = 4x frame size)
             */
            VADFrameProcessor(std::shared_ptr<audio::VADProvider> vadProvider, float lookbackMultiplier = 4.0f);
            virtual ~VADFrameProcessor() {}

            std::string getName() const override { return "VADFrameProcessor"; }
            bool initialize() override;

            void setMinFrameSize(size_t samples);

            void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

            void setConfig(const VADConfig& config) { _vadConfig = config; }
            const VADConfig& getConfig() const { return _vadConfig; }

            void setMetricsConfig(const va::metrics::VADMetricsConfig& config);
            const va::metrics::VADMetricsConfig& getVADMetricsConfig() const { return _metrics.config; }

            /**
             * @brief Set the shared ring buffer for reading audio data
             */
            void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) { _ringBuffer = ringBuffer; }

            protected:
            bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;

            // Process an audio frame for VAD analysis
            void processAudioFrame(std::shared_ptr<transport::AudioFrame> audioFrame);

            // Process control frames to handle pipeline state changes (e.g., flush events)
            void processControlFrame(std::shared_ptr<transport::ControlFrame> controlFrame);

            void processQuiet(const va::audio::VADProvider::VADResult* vadResult);
            void processStarting(const va::audio::VADProvider::VADResult* vadResult);
            void processOngoing(const va::audio::VADProvider::VADResult* vadResult);
            void processPaused(const va::audio::VADProvider::VADResult* vadResult);
            void processResuming(const va::audio::VADProvider::VADResult* vadResult);
            void processEnding(const va::audio::VADProvider::VADResult* vadResult);
            void processEnded(const va::audio::VADProvider::VADResult* vadResult);

            /**
             * @brief Merge adjacent VAD results within a pause window into single segments (avoids
             *        rapid state churn from brief pauses), and rebase offsets to the start of the
             *        first detected segment in this batch.
             * @param vadResults List of VAD results to merge
             * @param pauseWindowLengthSamples Max allowed pause between adjacent results to merge
             * @return Merged VAD results
             */
            std::vector<audio::VADProvider::VADResult> mergeVadResults(const std::vector<audio::VADProvider::VADResult>& vadResults, int pauseWindowLengthSamples) const;

            private:
            void switchToState(transport::VADState newState);
            void emitStateFrame(uint64_t speechStartOffset, uint64_t speechEndOffset, float confidence);

            std::shared_ptr<audio::VADProvider> _vadProvider;
            VADConfig _vadConfig;
            struct Metrics {
                va::metrics::VADMetricsConfig config;
                va::metrics::SamplerHandle    inference;    // inference latency ms
                va::metrics::SamplerHandle    speechDur;    // speech segment duration ms
                va::metrics::CounterHandle    segments;     // completed speech segments
                va::metrics::GaugeHandle      inferenceAlert;
            } _metrics;

            // State tracking
            transport::VADState _currentState{ transport::VADState::QUIET };
            transport::VADState _previousState{ transport::VADState::QUIET };

            // track the current absolute offset in the audio stream for accurate VAD state frames
            uint64_t _absoluteAudioOffset{ 0 };
            uint64_t _absoluteAudioPreOffset{ 0 };

            // Audio offsets - start is set once on speech onset, end moves forward
            // as speech continues and is emitted as speechEndOffset in VADStateFrames.
            struct SpeechOffsets {
                uint64_t start{ 0 };
                uint64_t end{ 0 };
            } _absoluteSpeechOffsets;

            // Absolute audio offset at the time the current state was entered.
            // Used as the base for all state-transition timers (pause duration,
            // resume confirmation, end confirmation) instead of storing
            // separate pause/resume offset fields.
            uint64_t _stateEntryAudioOffset{ 0 };

            // flag to track if we are currently in speech or not, to handle transitions between states correctly
            bool _isInSpeech{ false };

            uint64_t _minFrameSizeSamples{ 0 };
            float _lookbackMultiplier{ 4.0f }; // Multiplier for calculating lookback size based on frame size

            // Ring buffer for reading audio
            std::shared_ptr<AudioRingBuffer> _ringBuffer;
            std::vector<char> _vadAudioBuffer;  // Local buffer for VAD processing
        };

    } // namespace pipeline
} // namespace va
