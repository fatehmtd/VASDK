#ifndef SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP
#define SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class SupertonicInferenceImpl;

/**
 * @brief Voice synthesis provider backed by the Supertonic 2 local ONNX TTS engine.
 *
 * Implements GenericVoiceSynthesisProvider so it can be plugged directly into
 * TTSFrameProcessor in the VASDK pipeline.
 */
class SupertonicVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    /**
     * @param modelDir   Path to directory with ONNX models (tts.json, unicode_indexer.json, *.onnx)
     * @param stylePath  Path to a voice style JSON file (e.g. M1.json)
     * @param lang       Language code: en, ko, es, pt, fr  (default: "en")
     */
    SupertonicVoiceSynthesisProvider(const std::string& modelDir,
                                     const std::string& stylePath,
                                     const std::string& lang = "en");
    ~SupertonicVoiceSynthesisProvider() override;

    // --- GenericVoiceSynthesisProvider interface ---
    bool synthesizeSpeech(const std::string& text) override;
    void cancelSynthesis() override;
    bool startVoiceSynthesisProvider() override;
    void stopVoiceSynthesisProvider() override;
    bool isVoiceSynthesisProviderRunning() const override;
    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;
    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener) override;
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override;

private:
    std::shared_ptr<SupertonicInferenceImpl> impl_;
    va::audio::AudioConfig audioConfig_{};
    va::audio::VoiceSynthesisResultListener* resultListener_ = nullptr;
    bool _running = false;

    // Cached after initialize() - model sample rate is fixed for the session lifetime.
    int _modelSampleRate{0};

    // Persistent worker thread drains _textQueue sequentially.
    // synthesizeSpeech() enqueues; cancelSynthesis() clears queue + sets cancel flag.
    std::thread             _synthThread;
    std::atomic<bool>       _cancelSynth{false};
    std::atomic<bool>       _stopWorker{false};
    bool                    _inSynthesisSequence{false};
    std::queue<std::string> _textQueue;
    std::mutex              _queueMutex;
    std::condition_variable _queueCv;

    // Delivery thread - overlaps synthesis of chunk N+1 with delivery of chunk N.
    std::thread               _deliveryThread;
    std::mutex                _deliveryMutex;
    std::condition_variable   _deliveryCv;       // synthesis waits here: slot is empty
    std::condition_variable   _deliveryReadyCv;  // delivery waits here: slot is full
    std::optional<std::vector<float>> _deliverySlot;
    bool                      _deliveryDone{false};

    // Pre-allocated buffers reused across deliverAudio() calls (owned by _deliveryThread).
    std::vector<float>   _resampleBuf;
    std::vector<int16_t> _int16Buf;
    std::vector<int32_t> _int32Buf;
    std::vector<uint8_t> _uint8Buf;

    void workerLoop();
    void deliveryLoop();
    /// Synthesizes one queued text item. Returns true on completion, false if cancelled/error.
    bool synthesizeOne(const std::string& text);

    /// Hand off synthesized PCM to the delivery thread (blocks until slot is free).
    void handoffAudio(std::vector<float> wav);
    /// Drop any pending slot and unblock the synthesis thread (called on cancel/stop).
    void flushDeliverySlot();

    /// Convert float PCM ([-1,1]) to the format specified by audioConfig_ and deliver to listener.
    /// Uses pre-allocated member buffers. Must be called only from _deliveryThread.
    void deliverAudio(const std::vector<float>& pcm);

    /// Normalize text for TTS: spell out numbers, expand abbreviations, etc.
    static std::string normalizeText(const std::string& text);

public:
    // --- Synthesis quality / timing knobs ---

    /// Speaking speed multiplier (default 1.05). 1.0 = natural; higher = faster.
    void  setSpeed(float speed);
    float getSpeed() const;

    /// Seconds of silence appended after each chunk to separate utterances cleanly (default 0.3).
    void  setSilenceDuration(float seconds);
    float getSilenceDuration() const;

    /// Trim margin for vocoder output (default 0.0 = disabled).
    /// When > 0, each chunk is trimmed to sample_rate * duration * (1 + margin).
    /// Only enable if you need to remove the silent tail AND the duration predictor
    /// is accurate enough on your voice style to avoid clipping the last phoneme.
    void  setTrimMargin(float margin);
    float getTrimMargin() const;

    /// Number of diffusion sampling steps (default 10). Higher = better quality, slower synthesis.
    void setTotalSteps(int steps);
    int  getTotalSteps() const;

};

#endif // SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP