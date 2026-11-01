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
#include <deque>
#include <string>

namespace va {
namespace pipeline {

/**
 * @brief TTS processor - converts TextFrames into synthesized audio, one reply turn at a time.
 *
 * A reply may arrive as several TextFrames (isFinal=false sentence chunks while the LLM is still
 * generating, then one isFinal=true frame with the remaining text). The processor owns the turn:
 * it emits SynthesisState STARTED once, and COMPLETED once after the final frame's audio has
 * played out, regardless of how often the provider reports started/completed. Sentences go to
 * the provider one at a time unless it acceptsQueuedSynthesis().
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
    // Sends whatever the provider may take now. Takes _turnMutex; calls the provider without it.
    void pumpSentences();
    // Starts the drain wait once the reply is fully synthesized. Must be called without _turnMutex.
    void maybeStartDrain();
    void emitSynthesisState(transport::SynthesisState state, const std::string& errorMessage = {});
    void startDrainWait();
    void cancelDrainWait();
    void drainWaitLoop();

    std::shared_ptr<audio::GenericVoiceSynthesisProvider> _synthesisProvider;
    // Set on interrupt, cleared only by INTERRUPT_RESOLVED: drops text and audio from the
    // cancelled reply (a provider's synchronous onSynthesisCompleted() must not clear it).
    std::atomic<bool> _cancelPending{ false };

    // Current reply turn. Guarded by _turnMutex.
    std::mutex              _turnMutex;
    std::deque<std::string> _pendingSentences;  // not yet handed to the provider
    bool                    _providerBusy{ false };
    bool                    _textClosed{ false };  // final TextFrame of the reply received
    bool                    _turnActive{ false };  // STARTED emitted, COMPLETED not yet
    bool                    _draining{ false };
    bool                    _sentInTurn{ false };  // a sentence of this turn went to the provider

    std::function<void()>  _playbackFlush;
    std::function<void()>  _onAudioDrained;
    uint32_t               _hardwareBufferMarginMs{ 150 };

    // Duration tracking - reset at the start of each reply turn (guarded by _turnMutex)
    std::chrono::steady_clock::time_point _synthesisStartTime;
    // When the audio handed to the sink so far will have finished playing. Each chunk plays from
    // max(now, previous end): sentences can arrive with gaps while the LLM is still generating,
    // and the sink idles in between, so a plain "first chunk + total duration" would end early.
    std::chrono::steady_clock::time_point _playbackEndTime;

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
