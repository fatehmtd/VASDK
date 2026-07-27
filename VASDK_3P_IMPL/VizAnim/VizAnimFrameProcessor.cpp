#include "VizAnimFrameProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

namespace va {
    namespace thirdparty {

        // 
        // Construction / lifecycle
        // 

        VizAnimFrameProcessor::VizAnimFrameProcessor(const audio::AudioConfig& audioConfig, int numWorkingSamples)
            : _audioConfig(audioConfig),
            _vizAnimProvider(std::make_unique<VizAnimProvider>()),
            _numWorkingSamples(numWorkingSamples)
        {

        }

        bool VizAnimFrameProcessor::initialize()
        {
            if (!pipeline::FrameProcessor::initialize()) return false;
            if (!_vizAnimProvider->initialize(_audioConfig)) {
                spdlog::warn("[VizAnimFrameProcessor] Failed to initialize synthesis VizAnimProvider"
                    " - synthesis feature extraction disabled");
            }
            _workingBuffer.resize(_numWorkingSamples * _audioConfig.getSampleSizeInBytes());
            return true;
        }

        void VizAnimFrameProcessor::release()
        {
            _workingBuffer.clear();
            pipeline::FrameProcessor::release();
        }

        std::vector<VizAnimProvider::Features> VizAnimFrameProcessor::processAudioSamples(const char* input, size_t numSamples) const
        {
            return _vizAnimProvider->computeFeatures(input, static_cast<int>(numSamples));
        }

        /*************************************************************************************************************************/

        AudioInputVizAnimFrameProcessor::AudioInputVizAnimFrameProcessor(std::shared_ptr<va::AudioRingBuffer> ringBuffer,
            const audio::AudioConfig& audioConfig, int numWorkingSamples)
            : VizAnimFrameProcessor(audioConfig, numWorkingSamples),
            _ringBuffer(ringBuffer)
        {
        }

        void AudioInputVizAnimFrameProcessor::processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction)
        {
            VASDK_CTRACK_NAME("VizAnim::processAudioFrame");
            if (direction == va::pipeline::FlowDirection::DOWNSTREAM) {
                // push frame downstream first to minimize latency, then update the ring buffer with the new audio data
                pushDownstream(frame);

                if (frame->getType() == transport::FrameType::AUDIO) {
                    auto audioFrame = std::dynamic_pointer_cast<transport::AudioFrame>(frame);
                    if (audioFrame) {
                        processAudioFrame(audioFrame);
                    }
                }
            } else {
                pushUpstream(frame);
            }
        }

        bool AudioInputVizAnimFrameProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const
        {
            return frame->getType() == transport::FrameType::AUDIO;
        }

        void AudioInputVizAnimFrameProcessor::processAudioFrame(std::shared_ptr<transport::AudioFrame> frame)
        {
            // Each audio frame has ~2000 samples (125 ms at 16 kHz) but the FFT window
            // is 2048 samples, so reading only frame->numSamples would always yield 0
            // features.  Instead, read a sliding window of at least FFT_SIZE samples
            // ending at the last sample of the current frame.
            const long capacity   = _ringBuffer->getCapacity();
            const long fftSize    = static_cast<long>(VizAnimProvider::FFT_SIZE);
            const long windowSize = std::max(static_cast<long>(frame->numSamples), fftSize);

            // Ring-buffer position of the first sample AFTER this frame
            const long frameEnd   = (static_cast<long>(frame->ringbufferOffset) + frame->numSamples) % capacity;
            // Walk back windowSize samples (ring-buffer arithmetic)
            const long startOffset = (frameEnd - windowSize + capacity) % capacity;

            int numSamplesRead = _ringBuffer->readAbsolute(startOffset, _workingBuffer.data(), 0, static_cast<int>(windowSize));
            if(numSamplesRead > 0) {
                auto featuresVec = processAudioSamples(_workingBuffer.data(), static_cast<size_t>(numSamplesRead));

                if (!featuresVec.empty()) {
                    auto featuresFrame = std::make_shared<va::thirdparty::AudioFeaturesFrame>();
                    featuresFrame->featuresArray = std::move(featuresVec);
                    featuresFrame->source = va::thirdparty::AudioFeaturesSource::MIC;
                    featuresFrame->sequenceId = frame->sequenceId;
                    featuresFrame->frameId = frame->frameId + "_features";
                    featuresFrame->timestamp = frame->timestamp;
                    featuresFrame->sourceId = getName();
                    pushDownstream(featuresFrame);
                }
            } else {
                spdlog::warn("[AudioInputVizAnimFrameProcessor] Failed to read audio data from ring buffer at offset {}",
                    frame->absoluteOffset);
            }
        }

        /*************************************************************************************************************************/
        
        OutputAudioVizAnimFrameProcessor::OutputAudioVizAnimFrameProcessor(const audio::AudioConfig& audioConfig, int numWorkingSamples)
            : VizAnimFrameProcessor(audioConfig, numWorkingSamples)
        {
        }

        void OutputAudioVizAnimFrameProcessor::processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction)
        {
            VASDK_CTRACK_NAME("VizAnim::processSynthFrame");
            if(direction == va::pipeline::FlowDirection::DOWNSTREAM) {
                pushDownstream(frame);

                if(frame->getType() == transport::FrameType::SYNTHESIS) {
                    auto synthesisFrame = std::dynamic_pointer_cast<transport::SynthesisFrame>(frame);
                    if(synthesisFrame) {
                        processSynthesisFrame(synthesisFrame);
                    }
                }
            } else {
                pushUpstream(frame);
            }
        }

        bool OutputAudioVizAnimFrameProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const
        {
            return frame->getType() == transport::FrameType::SYNTHESIS;
        }

        void OutputAudioVizAnimFrameProcessor::processSynthesisFrame(std::shared_ptr<transport::SynthesisFrame> frame)
        {
            const char* audioDataPtr = frame->audioData.data();
            size_t numSamples = frame->audioData.size() / _audioConfig.getSampleSizeInBytes();

            auto featuresVec = processAudioSamples(audioDataPtr, numSamples);

            auto featuresFrame = std::make_shared<va::thirdparty::AudioFeaturesFrame>();
            featuresFrame->featuresArray = std::move(featuresVec);
            featuresFrame->source = va::thirdparty::AudioFeaturesSource::SYNTH;
            featuresFrame->sequenceId = frame->sequenceId;
            featuresFrame->frameId = frame->frameId + "_features";
            featuresFrame->timestamp = frame->timestamp;
            featuresFrame->sourceId = getName();

            pushDownstream(featuresFrame);
        }

} // namespace thirdparty
} // namespace va
