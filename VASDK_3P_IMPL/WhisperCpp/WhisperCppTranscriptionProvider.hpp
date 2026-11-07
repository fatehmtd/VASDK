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

namespace va::thirdparty {

/** Settings of WhisperCppTranscriptionProvider; every field but the model path has a default.
 *  Whisper runs a whole batch per pass, so partials are infrequent by default. */
struct WhisperCppTranscriptionOptions {
    /** A whisper.cpp GGML model file, .bin (VASDK_3P_IMPL/cmake/DownloadWhisperCppModel.cmake). */
    std::string modelPath;
    /** Silence after the last audio that finalizes the utterance when no end of speech arrives. */
    int idleTimeoutMs = 100;
    /** Shortest time between two partial results. */
    int partialIntervalMs = 500;
    /** New audio needed before another partial, in samples (8000: 500 ms at 16 kHz). */
    size_t minNewSamplesForPartial = 8000;
    /** New audio needed before the final pass re-runs, in samples (2000: 125 ms at 16 kHz). */
    size_t minNewSamplesForFinal = 2000;
    /** CPU threads for inference. */
    int threads = 4;
    /** Translate the speech to English instead of transcribing it. */
    bool translateToEnglish = false;
    /** Language code, or "auto" to detect it. */
    std::string language = "auto";
    /** Below whisper.cpp's 0.6 so short VAD-gated clips aren't dropped as "no speech". */
    float noSpeechThreshold = 0.3f;
    /** Every clip stands alone, so stale context from earlier segments can't leak into the prompt. */
    bool noContext = true;
};

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
    explicit WhisperCppTranscriptionProvider(WhisperCppTranscriptionOptions options);
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

private:
    struct InferenceResult {
        std::string text;
        float confidence = 0.0f;
        std::vector<va::audio::TranscriptionResult::Word> words;
    };

    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size);
    InferenceResult runInference(const std::vector<float>& audio);
    WhisperCppTranscriptionOptions _options;  // fixed after construction
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
};

} // namespace va::thirdparty
