#pragma once
#include <VASDK/GenericTranscriptionProvider.h>
#include "vasdk_whispercpp_export.h"
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <string>

struct whisper_context;

/**
 * @brief WhisperCppTranscriptionProvider
 * Implements GenericTranscriptionProvider using whisper.cpp for local,
 * offline speech-to-text transcription. Buffers incoming PCM audio,
 * runs batch inference in a dedicated thread, and delivers partial and
 * final TranscriptionResult notifications via the listener interface.
 *
 * Requirements:
 *  - Audio must be 16 kHz mono. Configure the pipeline accordingly.
 *  - A whisper.cpp GGML model file (.bin) must be supplied at construction.
 */
class VASDK_WHISPERCPP_EXPORT WhisperCppTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit WhisperCppTranscriptionProvider(const std::string& modelPath);
    ~WhisperCppTranscriptionProvider() override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    size_t writeAudioData(const uint8_t* data, size_t size) override;
    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override;
    void commitEndOfSpeech() override;

    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override;
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    /**
     * @brief InferenceConfig
     * Tunable parameters for the inference loop.
     * Whisper batch inference is heavier than streaming models, so partial
     * intervals and sample thresholds are set conservatively by default.
     */
    struct InferenceConfig {
        int idleTimeoutMs       = 100;
        int partialIntervalMs   = 500;    // whisper is slower; keep partials infrequent
        size_t minNewSamplesForPartial = 8000;  // ~500 ms @ 16 kHz
        size_t minNewSamplesForFinal   = 2000;  // ~125 ms @ 16 kHz
        int threads             = 4;
        bool translateToEnglish = false;
        std::string language    = "auto";
        // Lower than the whisper.cpp default (0.6) so short VAD-gated clips
        // are not suppressed as "no speech" before any tokens are emitted.
        float noSpeechThreshold = 0.3f;
        // Treat every clip as standalone; avoids stale context from prior
        // (possibly empty) segments polluting the decoder prompt.
        bool  noContext         = true;
    };

    void setInferenceConfig(const InferenceConfig& config);
    InferenceConfig getInferenceConfig() const;

private:
    struct InferenceResult {
        std::string text;
        float confidence = 0.0f;
        std::vector<va::audio::TranscriptionResult::Word> words;
    };

    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size);
    InferenceResult runInference(const std::vector<float>& audio);

    std::string _modelPath;
    whisper_context* _ctx = nullptr;

    va::audio::TranscriptionResultListener* _listener = nullptr;
    va::audio::AudioConfig _audioConfig{};
    std::atomic<bool> _running{ false };

    // Audio buffer and synchronisation
    std::vector<float> _buffer;
    std::mutex _mtx;
    std::mutex _inferenceMtx; // serialises concurrent writeFullAudioData calls
    std::condition_variable _cv;
    std::thread _thread;
    std::chrono::steady_clock::time_point _lastWriteTime;
    bool _hasNewAudio = false;
    bool _endOfSpeech = false;

    // Partial result cache (avoids re-inference when audio barely grew)
    std::string _cachedPartialResult;
    float _cachedPartialConfidence = 0.0f;
    std::vector<va::audio::TranscriptionResult::Word> _cachedPartialWords;
    size_t _lastInferSamples = 0;
    size_t _bufferSizeAtEndOfSpeech = 0;

    InferenceConfig _inferenceConfig;
};
