#pragma once

#include <VASDK/FrameProcessor.h>
#include <VASDK/AudioRingBuffer.h>
#include <VASDK/CommonAudioUtils.h>
#include "VizAnimProvider.h"
#include "AudioFeaturesFrame.h"
#include "vasdk_vizanim_export.h"

#include <memory>
#include <vector>

namespace va {
    namespace thirdparty {

        /**
         * @brief Pipeline stage that extracts audio features for live animation.
         *
         * VizAnimFrameProcessor observes both microphone audio (AudioFrame) and TTS
         * synthesis audio (SynthesisFrame) as they flow through the pipeline.  For each
         * source it runs the audio through a dedicated VizAnimProvider, emitting an
         * AudioFeaturesFrame downstream for every complete analysis window.
         *
         * All frames are forwarded downstream unchanged - the processor is a pure
         * side-observer.  Upstream (interrupt) frames are passed through without
         * modification.
         */
        class VASDK_VIZANIM_EXPORT VizAnimFrameProcessor : public pipeline::FrameProcessor {
            public:
            /**
             * @param audioConfig  Audio configuration for both microphone input (AudioFrames) and TTS synthesis output (SynthesisFrames).
             */
            explicit VizAnimFrameProcessor(const audio::AudioConfig& audioConfig, int numWorkingSamples);
            
            VizAnimFrameProcessor(const VizAnimFrameProcessor&) = delete;
            VizAnimFrameProcessor& operator=(const VizAnimFrameProcessor&) = delete;

            ~VizAnimFrameProcessor() override = default;

            std::string getName() const override { return "VizAnimFrameProcessor"; }

            bool initialize() override;
            void release() override;

            virtual void processFrame(std::shared_ptr<transport::Frame> frame,
                pipeline::FlowDirection direction) override = 0;

            protected:
            /**
             * @brief Extract features from raw audio samples using the VizAnimProvider.
             * @param input Pointer to raw audio samples.
             * @param numSamples Number of audio samples in the input buffer.
             * @return Vector of extracted features (one per analysis window).
             */
            std::vector<VizAnimProvider::Features> processAudioSamples(const char* input, size_t numSamples) const;

            protected:
            audio::AudioConfig  _audioConfig;
            std::unique_ptr<VizAnimProvider> _vizAnimProvider;
            std::vector<char>   _workingBuffer;
            int _numWorkingSamples;
        };


        /**
         * @brief Specialized VizAnimFrameProcessor for microphone audio (AudioFrames).
         */
        class VASDK_VIZANIM_EXPORT AudioInputVizAnimFrameProcessor : public VizAnimFrameProcessor {
        public:
            explicit AudioInputVizAnimFrameProcessor(std::shared_ptr<va::AudioRingBuffer> ringBuffer, 
                const audio::AudioConfig& audioConfig,
                 int numWorkingSamples);

            AudioInputVizAnimFrameProcessor(const AudioInputVizAnimFrameProcessor&) = delete;
            AudioInputVizAnimFrameProcessor& operator=(const AudioInputVizAnimFrameProcessor&) = delete;

            ~AudioInputVizAnimFrameProcessor() override = default;

            virtual void processFrame(std::shared_ptr<transport::Frame> frame,
                pipeline::FlowDirection direction) override;

            virtual bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
        private:
            void processAudioFrame(std::shared_ptr<transport::AudioFrame> frame);
        private:
            std::shared_ptr<va::AudioRingBuffer> _ringBuffer;
        };

        /**
         * @brief Specialized VizAnimFrameProcessor for TTS synthesis audio (SynthesisFrames).
         */
        class VASDK_VIZANIM_EXPORT OutputAudioVizAnimFrameProcessor : public VizAnimFrameProcessor {
        public:
            explicit OutputAudioVizAnimFrameProcessor(const audio::AudioConfig& audioConfig, int numWorkingSamples);

            OutputAudioVizAnimFrameProcessor(const OutputAudioVizAnimFrameProcessor&) = delete;
            OutputAudioVizAnimFrameProcessor& operator=(const OutputAudioVizAnimFrameProcessor&) = delete;

            ~OutputAudioVizAnimFrameProcessor() override = default;

            virtual void processFrame(std::shared_ptr<transport::Frame> frame,
                pipeline::FlowDirection direction) override;

            virtual bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;

        private:
            void processSynthesisFrame(std::shared_ptr<transport::SynthesisFrame> frame);
        };

    } // namespace thirdparty
} // namespace va
