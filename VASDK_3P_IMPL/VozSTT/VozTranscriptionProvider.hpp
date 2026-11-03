#pragma once
#include <VASDK/GenericTranscriptionProvider.h>
#include "vasdk_voz_stt_export.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @brief VozTranscriptionProvider
 * Implements GenericTranscriptionProvider with Desert Ant Labs' Voz model (Parakeet TDT
 * 0.6B v3 packaged for on-device runtimes, 25 European languages, word timestamps).
 *
 * Voz is only licensed through Desert Ant Labs' official SDKs (Kotlin, Swift, JS), which
 * own model download, inference and the usage telemetry the license requires. This
 * provider therefore does not run the model itself: it buffers PCM per utterance and hands
 * it to a platform-supplied VozRecognizer that calls the SDK (e.g. through JNI on Android).
 *
 * The SDK is batch-only, so the final result is produced at end of speech (or after
 * idleTimeoutMs without audio). Partials re-transcribe the whole utterance and are off by
 * default because every call decodes a full 15 s window.
 */
class VASDK_VOZ_STT_EXPORT VozTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    /**
     * Transcribes mono float samples in [-1, 1] at sampleRate (any rate; the SDK resamples).
     * Throws std::exception on failure; the provider reports it via onTranscriptionError.
     */
    using VozRecognizer = std::function<va::audio::TranscriptionResult(const std::vector<float>& samples, int sampleRate)>;

    explicit VozTranscriptionProvider(VozRecognizer recognizer);
    ~VozTranscriptionProvider() override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    size_t writeAudioData(const uint8_t* data, size_t size) override;
    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override;
    void commitEndOfSpeech() override;
    void clearAudioBuffer() override;

    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override;
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    /**
     * @brief InferenceConfig
     * Tunable parameters for the inference loop.
     */
    struct InferenceConfig {
        int idleTimeoutMs     = 500;
        int partialIntervalMs = 0;              // 0 disables partial results
        size_t minNewSamplesForPartial = 8000;  // ~500 ms @ 16 kHz
        size_t minNewSamplesForFinal   = 1600;  // ~100 ms @ 16 kHz
    };

    void setInferenceConfig(const InferenceConfig& config);
    InferenceConfig getInferenceConfig() const;

private:
    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size, std::vector<float>& out) const;
    va::audio::TranscriptionResult runInference(const std::vector<float>& audio);

    VozRecognizer _recognizer;
    bool _initialized = false;

    va::audio::TranscriptionResultListener* _listener = nullptr;
    va::audio::AudioConfig _audioConfig{};
    std::atomic<bool> _running{ false };

    // Audio buffer and synchronisation
    std::vector<float> _buffer;
    mutable std::mutex _mtx;
    std::condition_variable _cv;
    std::thread _thread;
    std::chrono::steady_clock::time_point _lastWriteTime;
    bool _hasNewAudio = false;
    bool _endOfSpeech = false;
    size_t _bufferSizeAtEndOfSpeech = 0;

    bool _discardCachedResult = false; // set by clearAudioBuffer, consumed by the inference thread

    // Partial result cache (avoids re-inference when audio barely grew); inference thread only
    va::audio::TranscriptionResult _cachedResult;
    size_t _lastInferSamples = 0;

    InferenceConfig _inferenceConfig;
};
