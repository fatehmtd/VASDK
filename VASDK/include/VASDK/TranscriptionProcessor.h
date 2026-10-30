#pragma once

#include "FrameProcessor.h"
#include "GenericTranscriptionProvider.h"
#include "AudioRingBuffer.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include <vector>
#include <mutex>
#include <string>

namespace va {
namespace pipeline {

/**
 * @brief Transcription processor - converts AudioFrames to TranscriptionFrames
 */
class VASDK_EXPORT TranscriptionFrameProcessor
    : public FrameProcessor
    , public audio::TranscriptionResultListener
    , public va::metrics::TimingObserverInterface {
public:
    TranscriptionFrameProcessor(std::shared_ptr<audio::GenericTranscriptionProvider> provider);

    std::string getName() const override { return "TranscriptionFrameProcessor"; }
    bool initialize() override;
    bool start() override;
    void stop() override;

    void setMetricsConfig(const va::metrics::TranscriptionMetricsConfig& config);
    const va::metrics::TranscriptionMetricsConfig& getTranscriptionMetricsConfig() const { return _metrics.config; }

    /**
     * @brief Configure the behavior for sending silence to the transcription provider
      * @param atStart whether to send silence at the start of speech
      * @param atEnd whether to send silence at the end of speech
      * @param durationMS duration of silence to send in milliseconds
     */
    void configSilenceBehavior(bool atStart, bool atEnd, int durationMS);

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

    // TranscriptionResultListener interface
    void onTranscriptionStarted() override;
    void onTranscriptionStopped() override;
    void onTranscriptionError(const std::string& error) override;
    void onPartialTranscription(const audio::TranscriptionResult& result) override;
    void onFinalTranscription(const audio::TranscriptionResult& result) override;

    // TimingObserverInterface - receives encoder/decoder latency from provider
    void onTiming(const char* stage, double elapsedMs) noexcept override;

    /**
     * @brief Set the shared ring buffer for reading audio data
     */
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) { _ringBuffer = ringBuffer; }

    /**
     * @brief Get the most recent partial transcription text (thread-safe).
     *
     * Returns an empty string if no partial has been produced yet in the
     * current speech segment.  Used by LLMProcessor to fire the LLM early
     * on low-latency THINKING transitions.
     */
    std::string getLatestPartialText() const;

    /**
     * @brief Get the confidence of the most recent partial transcription (thread-safe).
     * Returns 0.0 if no partial has been produced yet in the current speech segment.
     */
    float getLatestPartialConfidence() const;

    /**
     * @brief Set whether this processor is operating in streaming mode (i.e. processing continuous audio stream
     */
    void setStreamingMode(bool streaming) {
        _streamingMode = streaming;
    }

    /**
     * @brief Get whether this processor is operating in streaming mode
     */
    bool isStreamingMode() const {
        return _streamingMode;
    }

protected:
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void sendSilence(int durationMS) const;

    void processAudioFrame(std::shared_ptr<transport::AudioFrame> audioFrame);
    void processVADFrame(std::shared_ptr<transport::VADStateFrame> vadFrame);

    void processVADFrameStreaming(std::shared_ptr<transport::VADStateFrame> vadFrame);
    void processVADFrameNonStreaming(std::shared_ptr<transport::VADStateFrame> vadFrame);

private:
    // Streaming: sends ring-buffer audio from the pending speech start up to audioFrame's start.
    void flushPendingPreSpeech(const transport::AudioFrame& audioFrame);

    std::shared_ptr<audio::GenericTranscriptionProvider> _transcriptionProvider;
    bool _collectingAudio{false};
    // Streaming: lookback speech start from the STARTING frame, flushed on the next AUDIO
    // frame so the pre-speech context is contiguous with the incrementally-fed audio.
    bool     _hasPendingPreSpeech{false};
    uint64_t _pendingPreSpeechOffset{0};

    // Ring buffer for reading audio
    std::shared_ptr<AudioRingBuffer> _ringBuffer;
    std::vector<char> _transcriptionBuffer;  // Local buffer for transcription processing

    bool _sendStartSilence{false}; // should we send silence at the start of speech (to capture the beginning of speech better) 
    bool _sendEndSilence{false};   // should we send silence at the end of speech (to capture the end of speech better and trigger final results)
    int _silenceDurationMS{300};   // duration of silence to send in milliseconds
    bool _streamingMode{false};    // whether this processor is operating in streaming mode (i.e. processing continuous audio stream rather than discrete segments)

    // Latest partial text - updated on every onPartialTranscription, cleared on VAD start
    mutable std::mutex _latestPartialMutex;
    std::string        _latestPartialText;
    float              _latestPartialConfidence{0.0f};

    struct Metrics {
        va::metrics::TranscriptionMetricsConfig config;
        va::metrics::SamplerHandle encoderLatency;
        va::metrics::SamplerHandle decoderLatency;
        va::metrics::SamplerHandle confidence;
        va::metrics::SamplerHandle wordCount;
        va::metrics::CounterHandle transcriptions;
    } _metrics;
};

} // namespace pipeline
} // namespace va
