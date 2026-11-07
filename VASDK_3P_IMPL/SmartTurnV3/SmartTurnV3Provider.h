#ifndef SMART_TURN_V3_PROVIDER_H
#define SMART_TURN_V3_PROVIDER_H

#include <memory>
#include <string>
#include <vector>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/AudioRingBuffer.h>
#include <VASDK/CommonAudioUtils.h>
#include "vasdk_smartturn_v3_export.h"

namespace va {
    namespace thirdparty {
        class SmartTurnV3Impl;

        namespace whisper {
            constexpr int N_FFT = 400;
            constexpr int HOP_LENGTH = 160; // 10ms hop at 16kHz
            constexpr int N_MELS = 80;
        };

        /** Settings of SmartTurnV3Provider; every field but the model path has a default. */
        struct SmartTurnV3Options {
            /** smart_turn_v3.onnx (downloaded by VASDK_3P_IMPL/cmake/DownloadSmartTurnModel.cmake). */
            std::string modelPath;
            /** End-of-turn probability (0-1) that ends the turn. */
            float threshold = 0.5f;
            /** Longest stretch of the turn the model looks at, from its end backwards. */
            float maxAudioDurationSeconds = 8.0f;
            /** Audio frames after the VAD end the model waits for before its last evaluation. */
            int postAudioFrames = 3;
            /** Also evaluate while the user pauses, so a clearly finished turn ends at the pause
             *  (TurnDetectionProvider::setEvaluateOnPause). */
            bool evaluateOnPause = true;
        };

        /**
         * @brief SmartTurnV3 turn detection provider.
         *
         * Implements va::audio::TurnDetectionProvider so it can be injected into
         * TurnDetectionProcessor via its constructor.  The provider:
         *   - Emits TURN_START immediately when VAD transitions to STARTING.
         *   - Runs Whisper-feature + ONNX inference on post-VAD audio frames; emits
         *     TURN_END as soon as the model probability exceeds the threshold.
         *   - Falls back to emitting TURN_END on VAD ENDED if the model has not
         *     already fired.
         *
         * Usage:
         *   SmartTurnV3Options options;
         *   options.modelPath = modelsDir + "/smart_turn_v3.onnx";
         *   auto provider = std::make_shared<SmartTurnV3Provider>(options);
         *   provider->initialize(audioConfig, ringBuffer);
         *   auto turnProc = std::make_shared<TurnDetectionProcessor>(provider);
         */
        class VASDK_SMARTTURN_V3_EXPORT SmartTurnV3Provider : public va::audio::TurnDetectionProvider {
        public:
            explicit SmartTurnV3Provider(SmartTurnV3Options options = {});

            virtual ~SmartTurnV3Provider();

            /**
             * @brief Initialize with audio configuration and ring buffer.
             *
             * Must be called before the pipeline processes any frames.
             * Requires 16 kHz mono audio.
             */
            bool initialize(const va::audio::AudioConfig& audioConfig,
                std::shared_ptr<va::AudioRingBuffer> ringBuffer);

            //  TurnDetectionProvider interface 

            TurnEvaluation evaluate(const std::shared_ptr<transport::Frame> frame,
                bool agentIsSpeaking) override;

        private:
            TurnEvaluation handleVADStateFrame(const std::shared_ptr<transport::VADStateFrame>& frame,
                bool agentIsSpeaking);
            TurnEvaluation handleAudioFrame(const std::shared_ptr<transport::AudioFrame>& frame);

            void convertAudioToFloat(const char* audioData,
                size_t srcOffset,
                float* floatBuffer,
                size_t dstOffset,
                int numSamples);

        private:
            std::unique_ptr<SmartTurnV3Impl> impl_;
            float _threshold;
            bool _vadActive{ false };
            bool _vadOccured{ false };
            bool _paused{ false };  // VAD in PAUSED/ENDING
            size_t _vadStartOffset{ 0 };
            size_t _vadEndOffset{ 0 };
            float _maxAudioDurationSec{ 0.0f };
            int _numPostAudioFrames{ 0 };
            int _postAudioFramesProcessed{ 0 };
            int _nFFT{0};
            int _hopLength{0};
            int _nMels{0};
            std::string _modelPath;
            std::vector<float> _audioBuffer;
            std::vector<char>  _audioScratchBytes; // pre-allocated raw read buffer (maxDuration * bytesPerSample)
            std::shared_ptr<va::AudioRingBuffer> _audioRingBuffer;
            va::audio::AudioConfig _audioConfig;
        };

    } // namespace thirdparty
} // namespace va

#endif
