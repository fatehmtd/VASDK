#include "VASDK/AudioBufferProcessor.h"
#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

AudioBufferProcessor::AudioBufferProcessor(size_t bufferSize)
    : _bufferSize(bufferSize) {
}

bool AudioBufferProcessor::initialize() {
    if (!FrameProcessor::initialize()) return false;
    _ringBuffer = std::make_shared<AudioRingBuffer>(_bufferSize, sizeof(int16_t));
    return true;
}

void AudioBufferProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
    // AudioInputProcessor now writes directly to a shared ring buffer; this processor
    // is kept for backward compatibility and just passes frames through.
    pushDownstream(frame);
}

} // namespace pipeline
} // namespace va
