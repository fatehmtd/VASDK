#pragma once

#include "VASDK/AudioRingBuffer.h"
#include "VASDK/CommonAudioUtils.h"
#include "VASDK/Frame.h"
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

namespace va {
namespace transport {
namespace detail {

/**
 * @brief Write captured samples into the ring buffer (if any) and build the offset-only
 *        AudioFrame that VAD/STT read them back through. Shared by the audio-capturing transports.
 * @param absoluteOffset Running stream position in samples; advanced by numSamples.
 */
inline std::shared_ptr<AudioFrame> writeAudioFrame(AudioRingBuffer* ringBuffer,
                                                   const char* buffer, uint32_t numSamples,
                                                   const audio::AudioConfig& config,
                                                   uint64_t& absoluteOffset, uint64_t sequenceId,
                                                   const std::string& sourceId) {
    auto frame = std::make_shared<AudioFrame>();

    unsigned long ringbufferOffset = 0;
    unsigned long postWriteOffset = 0;

    if (ringBuffer) {
        ringbufferOffset = ringBuffer->getOffset();
        postWriteOffset = ringBuffer->write(buffer, 0, numSamples);
    } else {
        spdlog::debug("[{}] No ring buffer configured, skipping write", sourceId);
    }

    // Create lightweight frame (no samples data, just metadata)
    frame->numSamples = numSamples;
    frame->sampleRate = config.sampleRate;
    frame->channels = config.channels;
    frame->sampleSizeInBytes = config.getSampleSizeInBytes();
    frame->format = static_cast<int>(config.format);
    frame->absoluteOffset = absoluteOffset;
    frame->timestamp = Frame::getCurrentTimestamp();
    frame->sequenceId = sequenceId;
    frame->frameId = "_audio" + std::to_string(sequenceId);
    frame->sourceId = sourceId;
    frame->ringbufferOffset = ringbufferOffset;
    frame->postWriteOffset = postWriteOffset;

    // Update absolute offset (in samples, not bytes)
    absoluteOffset += numSamples;

    return frame;
}

} // namespace detail
} // namespace transport
} // namespace va
