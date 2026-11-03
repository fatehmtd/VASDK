#include "WakeWordProcessor.h"

#include "AudioRingBuffer.h"

#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

bool WakeWordProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    return frame->getType() == transport::FrameType::AUDIO;
}

void WakeWordProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
    if (direction != FlowDirection::DOWNSTREAM) {
        pushUpstream(frame);
        return;
    }
    pushDownstream(frame);
    const bool idle = !_agentIdle || _agentIdle();
    if (idle && !_wasIdle) _provider->reset();   // asleep again: start matching afresh
    _wasIdle = idle;
    if (!idle || !_ringBuffer) return;
    const auto& audio = static_cast<const transport::AudioFrame&>(*frame);
    if (audio.numSamples == 0) return;
    _samples.resize(audio.getSizeInBytes());
    const long start = _ringBuffer->normalizeOffset((long)audio.postWriteOffset - (long)audio.numSamples);
    const int read = _ringBuffer->readAbsolute(start, _samples.data(), 0, (int)audio.numSamples);
    if (read <= 0) return;
    if (auto detection = _provider->processAudio(_samples.data(), (size_t)read)) {
        spdlog::info("[WakeWordProcessor] \"{}\" detected (conf={:.2f})", detection->phrase, detection->confidence);
        auto trigger = std::make_shared<transport::TriggerFrame>();
        trigger->triggerType = transport::TriggerType::WAKE_WORD;
        trigger->triggerData = detection->phrase;
        trigger->confidence = detection->confidence;
        trigger->timestamp = transport::Frame::getCurrentTimestamp();
        trigger->sourceId = getName();
        pushDownstream(trigger);
    }
}

} // namespace pipeline
} // namespace va
