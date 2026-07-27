#pragma once

#include "FrameProcessor.h"
#include "GenericVoiceSynthesisProvider.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace va {
namespace pipeline {

/**
 * @brief TTS processor - converts TextFrames and TranscriptionFrames to Synthesized AudioFrames
 */
class VASDK_EXPORT TTSFrameProcessor : public FrameProcessor, public audio::VoiceSynthesisResultListener {
public:
    TTSFrameProcessor(std::shared_ptr<audio::GenericVoiceSynthesisProvider> provider);
    ~TTSFrameProcessor();

    std::string getName() const override { return "TTSFrameProcessor"; }
    bool initialize() override;
    bool start() override;
    void stop() override;

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

    /**
     * @brief Optional callback to immediately flush buffered audio on interrupt/error.
     */
    void setPlaybackFlush(std::function<void()> fn) { _playbackFlush = std::move(fn); }

    /**
     * @brief Optional callback fired when the audio drain completes (playback fully finished).
     *        Not called if synthesis was interrupted/cancelled.
     */
    void setOnAudioDrained(std::function<void()> fn) { _onAudioDrained = std::move(fn); }

    /**
     * @brief Extra margin added after the calculated audio duration to account for
     *        hardware buffer latency (SDL DMA buffer).  Default: 150ms.
     */
    void setHardwareBufferMarginMs(uint32_t ms) { _hardwareBufferMarginMs = ms; }

    void setMetricsConfig(const va::metrics::TTSMetricsConfig& config);
    const va::metrics::TTSMetricsConfig& getTTSMetricsConfig() const { return _metrics.config; }

    // VoiceSynthesisResultListener interface
    void onSynthesisStarted() override;
    void onSynthesisCompleted() override;
    void onSynthesisError(const std::string& error) override;
    void onAudioDataReceived(const char* data, size_t size) override;

protected:
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;

private:
    void handleAgentStateFrame(std::shared_ptr<transport::AgentStateFrame> frame);
    void synthesizeTextFrame(std::shared_ptr<transport::TextFrame> frame);
    void emitSynthesisState(transport::SynthesisState state, const std::string& errorMessage = {});
    void startDrainWait();
    void cancelDrainWait();
    void drainWaitLoop();

    std::shared_ptr<audio::GenericVoiceSynthesisProvider> _synthesisProvider;
    std::atomic<bool> _cancelPending{ false };

    std::function<void()>  _playbackFlush;
    std::function<void()>  _onAudioDrained;
    uint32_t               _hardwareBufferMarginMs{ 150 };

    // Duration tracking - reset on each synthesis start
    std::chrono::steady_clock::time_point _synthesisStartTime;
    // Set when the first audio chunk is delivered to the sink.
    // Audio begins playing at this point, so this is what the drain timer uses.
    std::chrono::steady_clock::time_point _playbackStartTime;
    std::atomic<int64_t>   _totalAudioDurationMs{ 0 };

    std::thread             _drainThread;
    std::mutex              _drainMutex;
    std::condition_variable _drainCv;
    bool                    _drainStop{ false };

    struct Metrics {
        va::metrics::TTSMetricsConfig config;
        va::metrics::SamplerHandle    ttfa;
        va::metrics::SamplerHandle    chunk;
        va::metrics::CounterHandle    requests;
        va::metrics::GaugeHandle      ttfaAlert;
    } _metrics;
    bool _firstChunkReceived{false};
};

} // namespace pipeline
} // namespace va
