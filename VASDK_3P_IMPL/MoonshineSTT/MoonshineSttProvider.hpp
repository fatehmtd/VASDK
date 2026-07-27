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
 * Implements GenericTranscriptionProvider using the Moonshine Base ONNX model
 * (encoder_model.onnx + decoder_model_merged.onnx + tokenizer.bin) via direct
 * OnnxRuntime C API inference. No Moonshine library dependency is required.
 *
 * Audio is accumulated in a float buffer. The inference thread runs the
 * encoder and then autoregressively decodes with KV-cache until an EOS token
 * or the maximum token budget is reached. Partial results are emitted
 * periodically; a final result is emitted on commitEndOfSpeech() or idle
 * timeout.
 *
 * Model source:
 *   https://huggingface.co/UsefulSensors/moonshine/tree/main/onnx/merged/base/quantized
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
        int    idleTimeoutMs             = 100;
        int    partialIntervalMs         = 100;
        size_t minNewSamplesForPartial   = 2000;  // ~125 ms at 16 kHz
        size_t minNewSamplesForFinal     = 2000;
        float  maxBufferSeconds          = 30.0f; // Moonshine base supports variable-length input up to ~30 s
        float  maxTokensPerSecond        = 6.5f;  // caps decode length = ceil(audio_sec * value)
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
    void appendAudioAsFloat(const uint8_t* data, size_t size);

    // Run a full encode+decode on audioData[0..numSamples). Returns decoded text
    // and a rough confidence estimate.
    struct InferResult { std::string text; float confidence = 0.0f; };
    InferResult runInference(const float* audioData, size_t numSamples);

    // --- model state ---
    std::string _modelDir;
    std::unique_ptr<MoonshineSttProviderImpl> _impl;

    // --- provider state ---
    va::audio::TranscriptionResultListener* _listener    = nullptr;
    va::audio::AudioConfig                  _audioConfig{};
    std::atomic<bool>                       _running{ false };

    // Audio accumulation buffer (preallocated to maxBufferSeconds * sampleRate)
    std::vector<float> _buffer;
    size_t             _bufferSize = 0;
    std::vector<float> _inferenceBuffer;
    size_t             _inferenceBufferSize = 0;

    std::mutex              _mtx;
    std::condition_variable _cv;
    std::thread             _thread;
    std::chrono::steady_clock::time_point _lastWriteTime;
    bool   _hasNewAudio              = false;
    bool   _endOfSpeech              = false;
    size_t _bufferSizeAtEndOfSpeech  = 0;

    // Cached partial result to avoid redundant inference runs
    std::string _cachedPartialResult;
    float       _cachedPartialConfidence = 0.0f;
    size_t      _lastInferSamples        = 0;

    InferenceConfig _inferenceConfig;

    va::metrics::TimingObserverInterface* _timingObserver = nullptr;
};
