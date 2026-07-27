#ifndef POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
#define POCKET_VOICE_SYNTHESIS_PROVIDER_HPP

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

class PocketTTSImpl;

/**
 * @brief Voice synthesis provider backed by Pocket TTS - a 5-model ONNX pipeline
 *        implementing autoregressive flow-matching TTS at 24 kHz.
 *
 * Implements GenericVoiceSynthesisProvider so it can be plugged directly into
 * TTSFrameProcessor in the VASDK pipeline.
 *
 * Model directory must contain:
 *   text_conditioner.onnx
 *   flow_lm_main_int8.onnx
 *   flow_lm_flow_int8.onnx
 *   mimi_decoder_int8.onnx
 *   mimi_encoder.onnx        (optional, only for voice cloning)
 *   tokenizer.model          (SentencePiece model)
 *   voices.bin               (pre-computed voice embeddings)
 *
 * voices.bin binary format (little-endian):
 *   uint32  num_voices
 *   per voice:
 *     char[32]  name (null-terminated ASCII, zero-padded)
 *     uint32    num_frames
 *     uint32    embed_dim   (always 1024)
 *     float32[num_frames * embed_dim]  embeddings
 */
class PocketVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    /**
     * @param modelDir   Path to directory containing all ONNX models + tokenizer + voices.bin
     * @param voiceName  Name of the voice to use (must match a name in voices.bin, e.g. "cosette")
     *                   Falls back to the first available voice if the name is not found.
     */
    explicit PocketVoiceSynthesisProvider(const std::string& modelDir,
                                          const std::string& voiceName = "cosette");
    ~PocketVoiceSynthesisProvider() override;

    // Non-copyable
    PocketVoiceSynthesisProvider(const PocketVoiceSynthesisProvider&)            = delete;
    PocketVoiceSynthesisProvider& operator=(const PocketVoiceSynthesisProvider&) = delete;

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

    // --- Synthesis quality / timing knobs ---

    /// Number of ODE flow-matching steps per latent frame (1–10).
    /// Higher = better quality audio, more compute per frame.
    /// Default: POCKET_TTS_DEFAULT_LSD_STEPS (6 desktop / 4 Android).
    void setLsdSteps(int steps);
    int  getLsdSteps() const;

    /// Seconds of silence appended after each utterance (default 0.3).
    void  setSilenceDuration(float seconds);
    float getSilenceDuration() const;

    /// Number of latent frames accumulated before each mimi_decoder call (default 8).
    /// Larger = more efficient decoding; smaller = lower first-audio latency.
    void setDecoderBatchFrames(int frames);
    int  getDecoderBatchFrames() const;

    /// Select a voice by name. Falls back to first available voice if not found.
    /// Thread-safe: applied before the next synthesizeOne() call.
    bool setVoice(const std::string& voiceName);
    std::string getVoice() const;

    /// Returns the list of available voice names parsed from voices.bin.
    std::vector<std::string> getAvailableVoices() const;

    /// Audio (ms) to pre-buffer before the first chunk reaches the listener -
    /// mitigates choppiness on platforms with aggressive CPU throttling.
    /// Default 1000ms; 0 disables pre-buffering.
    void setPreBufferedSamplesMS(int ms) {
        _preBufferedSamplesMS = std::max(0, ms);
    }

    int getPreBufferedSamplesMS() const {
        return _preBufferedSamplesMS;
    }

private:
    std::shared_ptr<PocketTTSImpl> _impl;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    bool _running = false;

    static constexpr int kModelSampleRate = 24000;

    // Synthesis worker thread (identical pattern to SupertonicVoiceSynthesisProvider)
    std::thread             _synthThread;
    std::atomic<bool>       _cancelSynth{false};
    std::atomic<bool>       _stopWorker{false};
    bool                    _inSynthesisSequence{false};
    std::queue<std::string> _textQueue;
    std::mutex              _queueMutex;
    std::condition_variable _queueCv;

    // Double-buffered delivery thread - overlaps synthesis with audio delivery
    std::thread               _deliveryThread;
    std::mutex                _deliveryMutex;
    std::condition_variable   _deliveryCv;        // synthesis waits here: slot is empty
    std::condition_variable   _deliveryReadyCv;   // delivery waits here: slot is full
    std::optional<std::vector<float>> _deliverySlot;
    bool                      _deliveryDone{false};
    int                      _preBufferedSamplesMS{1000}; // ms of audio to pre-buffer before first delivery, to help mitigate CPU throttling.

    // Pre-allocated format-conversion buffers (owned by _deliveryThread)
    std::vector<float>   _resampleBuf;
    std::vector<int16_t> _int16Buf;
    std::vector<int32_t> _int32Buf;
    std::vector<uint8_t> _uint8Buf;

    void workerLoop();
    void deliveryLoop();
    bool synthesizeOne(const std::string& text);
    void handoffAudio(std::vector<float> wav);
    void flushDeliverySlot();
    void deliverAudio(const std::vector<float>& pcm);

    // Text normalization: strip markdown, expand numbers, clean punctuation
    static std::string normalizeText(const std::string& text);
};

#endif // POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
