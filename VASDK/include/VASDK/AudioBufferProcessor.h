#pragma once

#include "FrameProcessor.h"
#include "AudioRingBuffer.h"
#include <memory>

namespace va {
namespace pipeline {

/**
 * @brief Audio ring buffer processor - stores audio in ring buffer
 */
class VASDK_EXPORT AudioBufferProcessor : public FrameProcessor {
public:
    AudioBufferProcessor(size_t bufferSize);
    ~AudioBufferProcessor() override = default;

    std::string getName() const override { return "AudioBufferProcessor"; }
    bool initialize() override;

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

    std::shared_ptr<AudioRingBuffer> getRingBuffer() const { return _ringBuffer; }

protected:
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override {
        return frame->getType() == transport::FrameType::AUDIO;
    }

private:
    std::shared_ptr<AudioRingBuffer> _ringBuffer;
    size_t _bufferSize;
};

} // namespace pipeline
} // namespace va
