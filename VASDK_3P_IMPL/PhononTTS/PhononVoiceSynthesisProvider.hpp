#ifndef PHONON_VOICE_SYNTHESIS_PROVIDER_HPP
#define PHONON_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace gradium::tts { class PhononTTS; }

/**
 * @brief Voice synthesis provider backed by Gradium's Phonon TTS (flow-matching transformer +
 *        Mimi decoder, 24 kHz mono), run by an in-tree C++ port of the inference on ggml
 *        (impl/), loading the Hugging Face checkpoint files as published.
 *
 * Implements GenericVoiceSynthesisProvider so it can be plugged directly into
 * TTSFrameProcessor in the VASDK pipeline.
 *
 * Model directory, laid out like the model's Hugging Face repo
 * (e.g. gradium/phonon-40m-multilingual):
 *   config.json
 *   tokenizer.model              (SentencePiece)
 *   model.q8.gguf
 *   default-voice.safetensors    (optional, exposed as the voice "default")
 *   voices/<name>.safetensors    (optional, one per voice, named after the file)
 *
 * Audio streams in chunks while the rest of the utterance is still being generated:
 * a 480 ms first chunk, then doubling up to setStreamChunkFrames().
 */
class PhononVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    /**
     * @param modelDir   Checkpoint folder (see above)
     * @param voiceName  Voice to speak in (file stem under voices/, or "default").
     *                   Empty or unknown falls back to "default", then the first voice.
     */
    explicit PhononVoiceSynthesisProvider(const std::string& modelDir,
                                          const std::string& voiceName = "",
                                          const std::string& language = "en");
    ~PhononVoiceSynthesisProvider() override;

    // Non-copyable
    PhononVoiceSynthesisProvider(const PhononVoiceSynthesisProvider&)            = delete;
    PhononVoiceSynthesisProvider& operator=(const PhononVoiceSynthesisProvider&) = delete;

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

    // --- Phonon knobs ---

    /// Select a voice by name. Thread-safe: applied before the next utterance.
    /// Returns false if no voice file with that name exists in the model directory.
    bool setVoice(const std::string& voiceName);
    std::string getVoice() const;

    /// Voice names found in the model directory (voices/*.safetensors, plus "default").
    std::vector<std::string> getAvailableVoices() const;

    /// Language the text is normalized as before synthesis: numbers, amounts, dates, symbols
    /// and addresses are spelled out the way Gradium's runtime does (en, fr, de, es, pt), or
    /// "none" to hand the text to the model as written. Default "en". Pick the language of the
    /// text, not of the voice. Returns false (and keeps the current one) for anything else.
    bool setLanguage(const std::string& language);
    std::string getLanguage() const;

    /// Which rewrite rules normalization runs: "default" (numbers, currency, dashed-digits,
    /// emails, urls, abbreviations, elongations), "all" (also phones, times, dates), "none", or
    /// a comma-separated list of rule names. Returns false (and keeps the current set) when the
    /// spec does not parse.
    bool setNormalizationRules(const std::string& rules);

    /// Sampling temperature (default 0.3). 0 is deterministic.
    void  setTemperature(float temperature);
    float getTemperature() const { return _temperature.load(); }

    /// Speaking-rate condition of the 40m checkpoints (default 0; negative = faster).
    void  setDurationDelta(float delta) { _durationDelta.store(delta); }
    float getDurationDelta() const { return _durationDelta.load(); }

    /// Largest audio chunk handed over, in 80 ms frames (default 12, ~1 s). The first
    /// chunk is always 6 frames so playback starts early.
    void setStreamChunkFrames(int frames);
    int  getStreamChunkFrames() const { return _streamChunkFrames.load(); }

    /// Longer text is spoken as groups of whole sentences of up to this many tokens
    /// (default 50, as Gradium's runtime). A sentence over 200 tokens is cut at a clause.
    void setMaxTokensPerChunk(int tokens);
    int  getMaxTokensPerChunk() const { return _maxTokensPerChunk.load(); }

    /// Seconds of silence appended after each completed synthesis sequence (default 0).
    void  setSilenceDuration(float seconds);
    float getSilenceDuration() const { return _silenceDuration; }

private:
    class StreamListener;

    std::string _modelDir;
    std::unique_ptr<gradium::tts::PhononTTS> _engine;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    bool _running = false;
    float _silenceDuration = 0.0f;

    static constexpr int kModelSampleRate = 24000;

    std::atomic<float> _temperature{0.3f};
    std::atomic<float> _durationDelta{0.0f};
    std::atomic<int>   _streamChunkFrames{12};
    std::atomic<int>   _maxTokensPerChunk{50};

    // Voice requested by the app, applied on the worker thread (the engine must not
    // switch voices while a stream is in flight).
    mutable std::mutex _settingsMutex;
    std::string _voice;
    std::string _loadedVoice;
    std::vector<std::string> _availableVoices;
    std::string _language = "en";   // normalization language, or "none"
    uint32_t    _ruleBits = 0;      // text::Rules::bits; set to the defaults in the constructor

    // Synthesis worker thread (same queue/sequence pattern as PocketVoiceSynthesisProvider)
    std::thread             _synthThread;
    std::atomic<bool>       _cancelSynth{false};
    std::atomic<bool>       _stopWorker{false};
    std::atomic<uint64_t>   _activeStream{0};
    bool                    _inSynthesisSequence{false};
    std::queue<std::string> _textQueue;
    std::mutex              _queueMutex;
    std::condition_variable _queueCv;

    // Streaming resampler state (24 kHz → pipeline rate), carried across chunks so chunk
    // boundaries don't click. Only touched from the engine's stream thread.
    std::vector<float> _rsHistory;
    double             _rsPos = 0.0;

    // Pre-allocated format-conversion buffers
    std::vector<float>   _resampleBuf;
    std::vector<int16_t> _int16Buf;
    std::vector<int32_t> _int32Buf;
    std::vector<uint8_t> _uint8Buf;

    void workerLoop();
    bool synthesizeOne(const std::string& text);
    bool applyPendingVoice();
    std::string voicePath(const std::string& name) const;
    void scanVoices();
    void onModelAudio(const float* pcm, size_t n);
    void resetResampler();
    void flushResampler();
    void resampleStream(const float* in, size_t n, std::vector<float>& out);
    void deliverAudio(const float* pcm, size_t n);
};

#endif // PHONON_VOICE_SYNTHESIS_PROVIDER_HPP
