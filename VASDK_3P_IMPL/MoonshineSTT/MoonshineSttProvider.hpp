#pragma once
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

namespace va::thirdparty {

// Forward declaration - ORT and tokenizer headers are hidden from consumers.
class MoonshineSttProviderImpl;

/** Settings of MoonshineSttProvider; every field but the model folder has a default. */
struct MoonshineSttOptions {
    /** Folder holding the streaming .ort graphs, streaming_config.json and tokenizer.bin
     *  (VASDK_3P_IMPL/cmake/DownloadMoonshineModel.cmake). */
    std::string modelDir;
    /** Silence on the input that finalizes the utterance. */
    int idleTimeoutMs = 350;
    /** Shortest time between two partial decodes. */
    int partialIntervalMs = 160;
};

/**
 * @brief MoonshineSttProvider
 *
 * Implements GenericTranscriptionProvider with the Moonshine streaming models
 * (Tiny / Small / Medium: frontend, encoder, adapter, cross_kv, decoder_kv .ort
 * graphs + streaming_config.json + tokenizer.bin). Inference uses Moonshine's
 * own streaming model code, compiled against VASDK's ONNX Runtime.
 *
 * Audio is encoded incrementally as it arrives, so each partial only pays for
 * the new audio plus a speculative re-decode. Partials are emitted every
 * partialIntervalMs; a final result is emitted on commitEndOfSpeech() or idle
 * timeout. Input must be 16 kHz mono.
 *
 * Model source:
 *   https://huggingface.co/UsefulSensors/moonshine-streaming
 */
class MoonshineSttProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit MoonshineSttProvider(MoonshineSttOptions options);
    ~MoonshineSttProvider() override;

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
     * @brief clearAudioBuffer
     * Discards in-progress audio accumulated for the current segment (e.g. on barge-in).
     */
    void clearAudioBuffer() override;

    void setTimingObserver(va::metrics::TimingObserverInterface* observer) override;

private:
    // --- internal helpers ---
    void inferenceLoop();
    void emit(const std::string& text, bool isFinal);

    // --- model state ---
    MoonshineSttOptions _options;  // fixed after construction
    std::unique_ptr<MoonshineSttProviderImpl> _impl;

    // --- provider state ---
    va::audio::TranscriptionResultListener* _listener    = nullptr;
    va::audio::AudioConfig                  _audioConfig{};
    std::atomic<bool>                       _running{ false };

    // Audio written since the inference thread last drained it (guarded by _mtx)
    std::vector<float> _incoming;

    std::mutex              _mtx;
    std::condition_variable _cv;
    std::thread             _thread;
    std::chrono::steady_clock::time_point _lastWriteTime;
    bool _endOfSpeech    = false;
    bool _resetRequested = false;

    va::metrics::TimingObserverInterface* _timingObserver = nullptr;
};

} // namespace va::thirdparty
