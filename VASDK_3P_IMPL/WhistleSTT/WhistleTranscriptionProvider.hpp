#pragma once
#include <VASDK/GenericTranscriptionProvider.h>
#include "vasdk_whistle_stt_export.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief WhistleTranscriptionProvider
 * Implements GenericTranscriptionProvider with Cactus Compute's Whistle model for local,
 * offline speech-to-text (en, de, fr, es, it, nl, pl, ~17 MB) on the CPU. Buffers incoming
 * PCM, re-transcribes the utterance for partial results, and transcribes it once more
 * for the final result at end of speech.
 *
 * Whistle runs on Cactus's prebuilt needle engine. On desktop the engine is a shared
 * library (libneedle3.dll / .so / .dylib) loaded at runtime from the model directory,
 * which DownloadWhistleSTT.cmake fills; on Android it is linked statically.
 *
 * The engine holds one speech model per process and is not thread-safe: all instances
 * share it and their inference calls are serialised.
 *
 * Requirements:
 *  - Audio must be 16 kHz mono. Configure the pipeline accordingly.
 *  - modelPath is a directory holding whistle.cact, or the path of a .cact file
 *    (e.g. a fine-tuned build from `needle whistle build`).
 */
class VASDK_WHISTLE_STT_EXPORT WhistleTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    /**
     * @param modelPath  Directory holding whistle.cact, or a .cact file path.
     * @param enginePath Optional explicit path of the engine library (desktop only). When
     *                   empty, the library is looked up next to the model, then on the
     *                   system search path.
     */
    explicit WhistleTranscriptionProvider(const std::string& modelPath, const std::string& enginePath = {});
    ~WhistleTranscriptionProvider() override;

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
     * Tunable parameters for the inference loop and the model.
     */
    struct InferenceConfig {
        int idleTimeoutMs       = 500;
        int partialIntervalMs   = 300;
        size_t minNewSamplesForPartial = 4800;  // ~300 ms @ 16 kHz
        size_t minNewSamplesForFinal   = 1600;  // ~100 ms @ 16 kHz
        // "en", "de", "fr", "es", "it", "nl" or "pl"; empty or "auto" detects per utterance.
        std::string language;
        // Names, product words and jargon to favour (keyword biasing).
        std::vector<std::string> keywords;
        // Word timings and probabilities; also what the result confidence is averaged from.
        bool wordTimestamps = true;
    };

    void setInferenceConfig(const InferenceConfig& config);
    InferenceConfig getInferenceConfig() const;

private:
    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size, std::vector<float>& out) const;
    va::audio::TranscriptionResult runInference(const std::vector<float>& audio);

    std::string _modelPath;
    std::string _enginePath;
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

    // Partial result cache (avoids re-inference when audio barely grew)
    va::audio::TranscriptionResult _cachedResult;
    size_t _lastInferSamples = 0;

    InferenceConfig _inferenceConfig;
};
