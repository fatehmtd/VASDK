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

namespace va::thirdparty::stt {

/** Settings of WhistleTranscriptionProvider; every field but the model path has a default. */
struct WhistleTranscriptionOptions {
    /** Directory holding whistle.cact, or the path of a .cact file (e.g. a fine-tuned build from
     *  `needle whistle build`). DownloadWhistleSTT.cmake fills the directory. */
    std::string modelPath;
    /** Explicit path of the engine library (desktop only). Empty: next to the model, then the
     *  system search path. */
    std::string enginePath;
    /** Silence after the last audio that ends the utterance when no end of speech arrives. */
    int idleTimeoutMs = 550;
    /** Time between two partial results. */
    int partialIntervalMs = 250;
    /** New audio needed before another partial, in samples (4000: 250 ms at 16 kHz). */
    size_t minNewSamplesForPartial = 4000;
    /** New audio needed before the final pass re-runs, in samples (1600: 100 ms at 16 kHz). */
    size_t minNewSamplesForFinal = 1600;
    /** "en", "de", "fr", "es", "it", "nl" or "pl"; empty or "auto" detects per utterance. */
    std::string language;
    /** Names, product words and jargon to favour (keyword biasing). */
    std::vector<std::string> keywords;
    /** Word timings and probabilities; also what the result confidence is averaged from. */
    bool wordTimestamps = true;
};

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
 * Audio must be 16 kHz mono.
 *
 *   WhistleTranscriptionOptions options;
 *   options.modelPath = modelsDir + "/whistle";
 *   auto stt = std::make_shared<WhistleTranscriptionProvider>(options);
 */
class VASDK_WHISTLE_STT_EXPORT WhistleTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit WhistleTranscriptionProvider(WhistleTranscriptionOptions options);
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
    /// The words the next answer may hold (an agent's question): favoured with the config's keywords.
    void setContextHints(const std::vector<std::string>& phrases) override;

private:
    void inferenceLoop();
    void appendAudioAsFloat(const uint8_t* data, size_t size, std::vector<float>& out) const;
    va::audio::TranscriptionResult runInference(const std::vector<float>& audio);

    WhistleTranscriptionOptions _options;  // modelPath resolved to the .cact file
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

    std::vector<std::string> _hints;   // under _mtx: from setContextHints
};

} // namespace va::thirdparty::stt
