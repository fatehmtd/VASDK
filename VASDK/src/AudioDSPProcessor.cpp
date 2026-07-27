#include "VASDK/AudioDSPProcessor.h"
#include "VASDK/Frame.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace va {
namespace pipeline {

AudioDSPProcessor::AudioDSPProcessor(
    std::shared_ptr<audio::GenericAudioDSPProvider> provider)
    : _provider(std::move(provider))
{
}

bool AudioDSPProcessor::initialize()
{
    if (!FrameProcessor::initialize()) return false;

    if (!_provider) {
        spdlog::error("[AudioDSPProcessor] No DSP provider supplied");
        return false;
    }
    if (!_rawBuffer) {
        spdlog::error("[AudioDSPProcessor] Raw audio ring buffer not set");
        return false;
    }
    if (!_cleanBuffer) {
        spdlog::error("[AudioDSPProcessor] Clean audio ring buffer not set");
        return false;
    }

    spdlog::info("[AudioDSPProcessor] Initialized with provider '{}'",
                 _provider->getName());
    return true;
}

void AudioDSPProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                     FlowDirection direction)
{
    VASDK_CTRACK_NAME("DSP::processFrame");
    if (direction != FlowDirection::DOWNSTREAM) {
        pushUpstream(frame);
        return;
    }

    auto audioFrame = std::dynamic_pointer_cast<transport::AudioFrame>(frame);
    if (!audioFrame) {
        pushDownstream(frame);
        return;
    }

    const int numSamples    = static_cast<int>(audioFrame->numSamples);
    const int bytesPerSample = static_cast<int>(
        _rawBuffer->getCapacityInBytes() / _rawBuffer->getCapacity());
    const size_t totalBytes = static_cast<size_t>(numSamples) * bytesPerSample;

    // Ensure scratch buffer is large enough for raw read + processed write
    _scratch.resize(totalBytes * 2);
    char* rawData   = _scratch.data();
    char* cleanData = _scratch.data() + totalBytes;

    // Read raw audio from the main ring buffer
    if (_rawBuffer->readAbsolute(static_cast<long>(audioFrame->ringbufferOffset),
                                 rawData, 0, numSamples) < 0) {
        spdlog::warn("[AudioDSPProcessor] readAbsolute failed - writing silence to clean buffer");
        std::memset(cleanData, 0, totalBytes);
    } else {
        _provider->processAudio(rawData, cleanData, static_cast<size_t>(numSamples));
    }

    // Advance clean ring buffer in lock-step with the raw ring buffer
    _cleanBuffer->write(cleanData, 0, numSamples);

    pushDownstream(frame);
}

} // namespace pipeline
} // namespace va
