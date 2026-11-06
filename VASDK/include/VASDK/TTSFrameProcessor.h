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
#include <deque>
#include <vector>
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
    std::string getProviderName() const override {
        std::lock_guard<std::mutex> lock(_providerMutex);
        return _synthesisProvider ? _synthesisProvider->getName() : std::string();
    }
    /**
     * Speaks with `provider` from the next reply on (a reply already playing finishes with the
     * one before): that one is stopped, this one started. A local voice for when the network
     * drops, and back. A reply the old provider's error cut off is spoken again with the new
     * one, from its first part the user hadn't fully heard (unless the user spoke meanwhile, or
     * another reply came). Thread-safe.
     */
    void replaceProvider(std::shared_ptr<audio::GenericVoiceSynthesisProvider> provider);
    /** The words of the reply being spoken, or of the one just finished: what echo from the speakers would be. Thread-safe. */
    std::string currentReplyText();
    /** Called, on the provider's thread, when the provider reports an error (a lost connection). */
    void setOnProviderError(std::function<void(const std::string& error)> callback) { _onProviderError = std::move(callback); }
    bool initialize() override;
    bool start() override;
    void stop() override;

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

    /**
     * @brief Optional callback to immediately flush buffered audio on interrupt/error.
     */
    void setPlaybackFlush(std::function<void()> fn) { _playbackFlush = std::move(fn); }

    /**
     * @brief Optional callback that ducks or pauses playback while a barge-in is pending
     *        (AgentStateTransition::BARGE_IN_PENDING) and releases it (held = false) when the
     *        speech turns out to be a back-channel or the turn ends. Wire it to the transport's
     *        holdOutputAudio(); a Pause also holds back the drain timer so the turn doesn't end early.
     */
    using PlaybackHold = std::function<void(transport::PendingPlaybackMode, bool held, float duckGainDecibels)>;
    void setPlaybackHold(PlaybackHold playbackHold) { _playbackHold = std::move(playbackHold); }
    /**
     * @brief How playback is held while a barge-in is pending. In Duck mode the duck lasts at least
     *        duckDurationMs from when the user started speaking over the agent, so a back-channel
     *        doesn't pump the volume back up after ~0.3 s; a committed barge-in or the reply ending
     *        releases it at once. Pause mode always resumes as soon as the speech is dismissed.
     */
    void setPendingPlayback(transport::PendingPlaybackMode pendingPlaybackMode, float duckGainDecibels,
                            uint32_t duckDurationMs = 2500) {
        _pendingPlaybackMode = pendingPlaybackMode;
        _duckGainDecibels = duckGainDecibels;
        _duckDurationMs = duckDurationMs;
    }

    /**
     * @brief Milliseconds of the current reply still to be played, or -1 while the reply is
     *        still being synthesized or no reply is playing. Feeds AgentStateProcessor's tail-overlap rule.
     */
    int getRemainingReplyPlaybackMs();

    /**
     * @brief Milliseconds left of a part the user can't interrupt (TextFrame::interruptible false)
     *        that is playing now, or 0. Such a part is synthesized on its own, so its audio is known
     *        to start and end. Feeds AgentStateProcessor::setProtectedPlaybackMsProvider.
     */
    int getProtectedPlaybackMs();

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
    void holdPlayback(bool held);
    // Releases the duck at holdStart + duckDurationMs (or now, if that has passed).
    void releaseDuckAfterMinimumDuration();
    void holdReleaseLoop();

    void applyReplacement();
    // A reply cut off by a provider error, spoken again once another provider took over.
    // On the pipeline thread; true when it was (or there's nothing to speak again).
    void respeakCutOff();

    std::shared_ptr<audio::GenericVoiceSynthesisProvider> _synthesisProvider;   // written under _providerMutex, between replies
    mutable std::mutex _providerMutex;
    std::shared_ptr<audio::GenericVoiceSynthesisProvider> _replacement;         // under _providerMutex
    std::function<void(const std::string&)> _onProviderError;
    // Set on interrupt, cleared only by INTERRUPT_RESOLVED: drops text and audio from the
    // cancelled reply (a provider's synchronous onSynthesisCompleted() must not clear it).
    std::atomic<bool> _cancelPending{ false };

    // Current reply turn. Guarded by _turnMutex.
    std::mutex              _turnMutex;
    struct Sentence {
        std::string text;          // what the provider is given: plain text, or SSML when it supportsSsml()
        bool interruptible = true;
        va::audio::SpeechStyle style;   // another voice or pace for it
    };
    std::deque<Sentence>    _pendingSentences;  // not yet handed to the provider
    // The parts of this reply handed to the provider, and when each one's audio ends playing
    // (known once synthesized): what a provider error cuts off is from the first unheard one.
    struct SentPart {
        Sentence sentence;
        bool synthesized = false;
        std::chrono::steady_clock::time_point endsAt;
    };
    std::vector<SentPart>   _sentParts;
    std::string             _lastReplyText;     // the reply that finished last, until the next one starts
    std::deque<Sentence>    _cutOff;            // to speak again with the next provider
    bool                    _cutOffClosed{ false };   // the cut-off reply had all its text
    std::shared_ptr<audio::GenericVoiceSynthesisProvider> _cutOffBy;   // the provider that failed
    bool                    _providerBusy{ false };
    bool                    _textClosed{ false };  // final TextFrame of the reply received
    bool                    _turnActive{ false };  // STARTED emitted, COMPLETED not yet
    bool                    _draining{ false };
    bool                    _sentInTurn{ false };  // a sentence of this turn went to the provider
    // A part that can't be interrupted: sent, its synthesis not yet completed. Its audio plays
    // from _protectedStart to _protectedEnd, which grows with each chunk received meanwhile.
    bool                                  _protectedSending{ false };
    std::chrono::steady_clock::time_point _protectedStart;
    std::chrono::steady_clock::time_point _protectedEnd;

    std::function<void()>  _playbackFlush;
    std::function<void()>  _onAudioDrained;
    uint32_t               _hardwareBufferMarginMs{ 150 };

    // Duration tracking - reset at the start of each reply turn (guarded by _turnMutex)
    std::chrono::steady_clock::time_point _synthesisStartTime;
    // When the audio handed to the sink so far will have finished playing. Each chunk plays from
    // max(now, previous end): sentences can arrive with gaps while the LLM is still generating,
    // and the sink idles in between, so a plain "first chunk + total duration" would end early.
    std::chrono::steady_clock::time_point _playbackEndTime;
    // Pause hold: the playback clock stops, so the remaining time is frozen and grows with new chunks.
    bool                      _playbackPaused{ false };
    std::chrono::microseconds _playbackRemainingWhilePaused{ 0 };

    PlaybackHold                   _playbackHold;
    transport::PendingPlaybackMode _pendingPlaybackMode{ transport::PendingPlaybackMode::Duck };
    float                          _duckGainDecibels{ -12.0f };
    uint32_t                       _duckDurationMs{ 2500 };
    // Deferred duck release. The hold callback is only called under _holdReleaseMutex, so a release
    // firing on the timer thread can't land after a new BARGE_IN_PENDING has ducked again.
    std::thread                           _holdReleaseThread;
    std::mutex                            _holdReleaseMutex;
    std::condition_variable               _holdReleaseCv;
    bool                                  _holdReleaseStop{ false };
    bool                                  _holdReleaseScheduled{ false };
    std::chrono::steady_clock::time_point _holdStartTime;
    std::chrono::steady_clock::time_point _holdReleaseTime;

    std::thread             _drainThread;
    std::mutex              _drainThreadMutex;  // start/cancel run on the pipeline, provider and stop() threads
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
