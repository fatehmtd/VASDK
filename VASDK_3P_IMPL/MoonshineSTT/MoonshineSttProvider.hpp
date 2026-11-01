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
// Forward declaration - ORT and tokenizer headers are hidden from consumers.
class MoonshineSttProviderImpl;

class MoonshineSttProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit MoonshineSttProvider(const std::string& modelDir);
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
     * @brief InferenceConfig
     * Configurable parameters for the inference loop.
     */
    struct InferenceConfig {
        int idleTimeoutMs     = 350;  // finalize the utterance after this much silence on the input
        int partialIntervalMs = 200;  // minimum spacing between partial decodes
    };

    void setInferenceConfig(const InferenceConfig& config);
    InferenceConfig getInferenceConfig() const;

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
    std::string _modelDir;
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

    InferenceConfig _inferenceConfig;

    va::metrics::TimingObserverInterface* _timingObserver = nullptr;
};
