#pragma once

#include "FrameProcessor.h"
#include "GenericAudioDSPProvider.h"
#include "AudioRingBuffer.h"
#include <memory>
#include <vector>

namespace va {
namespace pipeline {

/**
 * @brief Pipeline stage that applies a GenericAudioDSPProvider to every
 *        audio frame and writes the processed result to a secondary
 *        "clean" ring buffer.
 *
 * AudioDSPProcessor sits between TransportInputProcessor and VADFrameProcessor
 * in the pipeline.  For each AudioFrame it:
 *   1. Reads raw audio from the raw ring buffer at frame->ringbufferOffset.
 *   2. Passes it through the GenericAudioDSPProvider (e.g. RNNoiseDSPProvider).
 *   3. Writes the processed samples to the clean ring buffer.
 *   4. Forwards the original AudioFrame downstream unchanged so that
 *      downstream processors which use the clean ring buffer will read
 *      DSP-processed audio at the same ringbufferOffset.
 *
 * Both ring buffers must be created with identical capacity and sample size.
 * The clean ring buffer is written in lock-step with the raw ring buffer -
 * AudioFrame::ringbufferOffset is therefore valid for both buffers.
 *
 * Usage:
 * @code
 *   auto provider = std::make_shared<RNNoiseDSPProvider>();
 *   provider->initialize(audioConfig);
 *
 *   auto dspProcessor = std::make_shared<AudioDSPProcessor>(provider);
 *   dspProcessor->setRawAudioRingBuffer(sharedRingBuffer);
 *   dspProcessor->setCleanAudioRingBuffer(cleanRingBuffer);
 *
 *   // In pipeline: ... → dspProcessor → vadProcessor → ...
 *   // VAD/STT use cleanRingBuffer; debug loggers may use sharedRingBuffer.
 * @endcode
 */
class VASDK_EXPORT AudioDSPProcessor : public FrameProcessor {
public:
    explicit AudioDSPProcessor(
        std::shared_ptr<audio::GenericAudioDSPProvider> provider);

    std::string getName() const override { return "AudioDSPProcessor"; }

    bool initialize() override;

    void processFrame(std::shared_ptr<transport::Frame> frame,
                      FlowDirection direction) override;

    /** @brief Ring buffer containing raw (unprocessed) audio from LocalTransport. */
    void setRawAudioRingBuffer(std::shared_ptr<AudioRingBuffer> buffer) {
        _rawBuffer = std::move(buffer);
    }

    /** @brief Ring buffer that will receive DSP-processed audio. */
    void setCleanAudioRingBuffer(std::shared_ptr<AudioRingBuffer> buffer) {
        _cleanBuffer = std::move(buffer);
    }

protected:
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override {
        return frame->getType() == transport::FrameType::AUDIO;
    }

private:
    std::shared_ptr<audio::GenericAudioDSPProvider> _provider;
    std::shared_ptr<AudioRingBuffer>                _rawBuffer;
    std::shared_ptr<AudioRingBuffer>                _cleanBuffer;
    std::vector<char>                               _scratch; // read + write scratch
};

} // namespace pipeline
} // namespace va
