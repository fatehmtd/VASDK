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
#include <string>

namespace va::thirdparty {

// Forward declaration; handles the actual Parakeet model inference.
class ParakeetInferenceProviderImpl;

/** Settings of ParakeetTranscriptionProvider; every field but the model folder has a default.
 *  Sample counts assume 16 kHz. */
struct ParakeetTranscriptionOptions {
    /** Folder holding the Parakeet ONNX files (VASDK_3P_IMPL/cmake/DownloadParakeetModel.cmake). */
    std::string modelDir;
    /** Silence after the last audio that finalizes the utterance when no end of speech arrives. */
    int idleTimeoutMs = 550;
    /** Shortest time between two partial results. */
    int partialIntervalMs = 250;
    /** Partials re-run only the new audio; off by default for accuracy. */
    bool enableIncrementalPartials = false;
    /** New audio needed before another partial (8000: 500 ms). */
    size_t minNewSamplesForPartial = 8000;
    /** The final pass is skipped when the buffer grew by less than this since the last partial (16000: 1 s). */
    size_t minNewSamplesForFinal = 16000;
    /** When the cached partial's confidence reaches highConfShortcutThreshold and fewer than this many
     *  samples arrived after it, it becomes the final result without another inference, which avoids
     *  multi-second stalls on Android. 0 disables the shortcut. */
    size_t highConfShortcutSamples = 16000;
    /** Confidence (0-1) the cached partial needs for that shortcut. */
    float highConfShortcutThreshold = 0.85f;
    /** Tail of the previous utterance prepended to each final pass, for decoder context (8000: 500 ms). */
    size_t contextSamples = 8000;
    /** Words that start within this of the context's end belong to the context and are dropped;
     *  must stay below the VAD's maxSpeechEndDurationMs. */
    int64_t contextBoundaryToleranceMs = 100;
    /** Capacity of the preallocated audio buffer. */
    float maxBufferSeconds = 90.0f;
    /** Silence appended to each chunk for decoder stability at its end. */
    float silencePaddingSeconds = 1.0f;
    /** Tolerance for words at chunk boundaries; must exceed contextBoundaryToleranceMs. */
    int overlapBoundaryToleranceMs = 1000;
    /** Tolerance for words at the end of the last chunk, against hallucinations in the silence pad. */
    int endPaddingToleranceMs = 1000;
};

/**
 * @brief ParakeetTranscriptionProvider
 * Implements GenericTranscriptionProvider using the Parakeet model. Manages audio
 * buffering and runs an inference loop on a separate thread, notifying a
 * TranscriptionResultListener of partial and final results.
 */
class ParakeetTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
    public:
    explicit ParakeetTranscriptionProvider(ParakeetTranscriptionOptions options);
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

    ParakeetTranscriptionOptions _options;  // fixed after construction
    std::unique_ptr<ParakeetInferenceProviderImpl> _parakeetInferenceProviderImpl;
    std::atomic<va::audio::TranscriptionResultListener*> _listener{ nullptr };
    std::atomic<bool> _allowCallbacks{ false };
    va::audio::AudioConfig _audioConfig{};
    std::atomic<bool> _running{ false };

    // Audio buffering and inference thread.
    // _buffer is preallocated to ParakeetTranscriptionOptions::maxBufferSeconds at start time.
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

};

} // namespace va::thirdparty
