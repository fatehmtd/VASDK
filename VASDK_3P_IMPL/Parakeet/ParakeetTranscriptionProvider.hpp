#pragma once
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

// Forward declaration; handles the actual Parakeet model inference.
class ParakeetInferenceProviderImpl;

/**
 * @brief ParakeetTranscriptionProvider
 * Implements GenericTranscriptionProvider using the Parakeet model. Manages audio
 * buffering and runs an inference loop on a separate thread, notifying a
 * TranscriptionResultListener of partial and final results.
 */
class ParakeetTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
    public:
    explicit ParakeetTranscriptionProvider(const std::string& modelDir);
    ~ParakeetTranscriptionProvider() override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    size_t writeAudioData(const uint8_t* data, size_t size) override;
    void commitEndOfSpeech() override;

    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override;

    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override;
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    /**
     * @brief InferenceConfig
     * Tunable timeouts and thresholds for the inference loop's responsiveness.
     */
    struct InferenceConfig {
        int   idleTimeoutMs            = 100;
        int   partialIntervalMs        = 100;
        // Accuracy-first default: partials use full inference unless explicitly enabled.
        bool  enableIncrementalPartials = false;
        size_t minNewSamplesForPartial = 2000;  // ~125 ms at 16 kHz
        size_t minNewSamplesForFinal   = 2000;  // skip final re-inference if buffer barely grew
        // When the cached partial has confidence >= highConfShortcutThreshold and the
        // buffer grew by fewer than highConfShortcutSamples since that partial, emit it
        // as the final result instead of re-running the (expensive) final inference.
        // Avoids multi-second stalls on Android when the partial already captured the
        // full utterance. Set highConfShortcutSamples = 0 to disable.
        size_t highConfShortcutSamples    = 16000; // ~1 s at 16 kHz
        float  highConfShortcutThreshold  = 0.85f; // minimum cached-partial confidence
        size_t contextSamples          = 32000; // ~2 s at 16 kHz; tail prepended to each final for decoder context
        int64_t contextBoundaryToleranceMs = 100; // tolerance for discarding context-prefix words;
                                                   // must be < VAD maxSpeechEndDurationMs
        float maxBufferSeconds         = 90.0f; // capacity of the preallocated audio accumulation buffer
        float silencePaddingSeconds       = 0.5f;  // extra silence appended per chunk for decoder stability at boundaries

        // Tolerance for discarding words at chunk boundaries; must be > contextBoundaryToleranceMs
        // so valid words aren't dropped when the context tail is short/disabled. 1000 ms works
        // well in practice for Parakeet TDT with a 2 s context tail.
        int overlapBoundaryToleranceMs = 1000;
        int endPaddingToleranceMs      = 1000; // tolerance for words at the end of the last chunk; avoids decoder hallucinations from the silence pad
    };

    /**
     * @brief setInferenceConfig
     * Sets the inference configuration parameters for the transcription provider.
     */
    void setInferenceConfig(const InferenceConfig& config);

    /**
     * @brief getInferenceConfig
     * Gets the current inference configuration parameters for the transcription provider.
     * @return The current InferenceConfig struct with the provider's inference parameters.
     */
    InferenceConfig getInferenceConfig() const;

    /**
     * @brief Discard all in-progress audio accumulated so far for the current segment.
     *
     * Call this on barge-in to throw away audio that was captured while TTS was
     * still playing through the speakers (and thus contains speaker echo).  The
     * context tail from the previous completed turn is intentionally preserved so
     * the decoder still has prior-speech context for the next inference.
     */
    void clearAudioBuffer() override;

    /**
     * @brief Set a timing observer to receive per-stage inference latency events.
     * Stages reported: "encoder", "decoder".
     */
    void setTimingObserver(va::metrics::TimingObserverInterface* observer) override;

    private:
    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size);

    std::string _modelDir;
    std::unique_ptr<ParakeetInferenceProviderImpl> _parakeetInferenceProviderImpl;
    std::atomic<va::audio::TranscriptionResultListener*> _listener{ nullptr };
    std::atomic<bool> _allowCallbacks{ false };
    va::audio::AudioConfig _audioConfig{};
    std::atomic<bool> _running{ false };

    // Audio buffering and inference thread.
    // _buffer is preallocated to InferenceConfig::maxBufferSeconds at start time.
    // _bufferSize tracks the number of valid samples written so far.
    // On each final inference the two buffers are swapped (O(1)) so that the
    // inference thread can run without holding the mutex.
    std::vector<float> _buffer;
    size_t             _bufferSize = 0;
    std::vector<float> _inferenceBuffer;   // preallocated snapshot passed to infer()
    size_t             _inferenceBufferSize = 0;
    std::mutex _mtx;
    std::condition_variable _cv;
    std::thread _thread;
    std::chrono::steady_clock::time_point _lastWriteTime;
    bool _hasNewAudio = false;
    bool _endOfSpeech = false;

    // Partial transcription result cache
    std::string _cachedPartialResult;
    std::vector<va::audio::TranscriptionResult::LanguageTag> _cachedPartialLanguage;
    float _cachedPartialConfidence = 0.0f;
    std::vector<va::audio::TranscriptionResult::Word> _cachedPartialWords;
    size_t _lastInferSamples = 0;
    // _bufferSize captured AFTER the last partial inference finished (not before
    // it started like _lastInferSamples).  Used by the high-confidence shortcut
    // to measure only the audio that arrived after the partial result was ready.
    size_t _sampleCountAtLastPartial = 0;
    size_t _bufferSizeAtEndOfSpeech = 0; // buffer size when commitEndOfSpeech was called

    // Reusable context+audio buffer for final inference - pre-allocated in
    // startTranscriptionProvider() to avoid a per-utterance heap allocation.
    std::vector<float> _audioWithContext;

    // Tail audio kept from the end of the last finalized segment.
    // Prepended to the next final inference so the decoder has prior-speech
    // context and doesn't start cold at a hard VAD boundary.
    std::vector<float> _contextTailBuffer;

    InferenceConfig _inferenceConfig;
};
