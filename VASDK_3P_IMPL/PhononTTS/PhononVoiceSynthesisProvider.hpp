#ifndef PHONON_VOICE_SYNTHESIS_PROVIDER_HPP
#define PHONON_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va::thirdparty::tts {

namespace phonon { class PhononTTS; }

/** Settings of PhononVoiceSynthesisProvider; every field but the model folder has a default. */
struct PhononVoiceSynthesisOptions {
    /** Checkpoint folder, laid out like the model's Hugging Face repo (see the class comment;
     *  VASDK_3P_IMPL/cmake/DownloadPhononTTSModel.cmake). */
    std::string modelDir;
    /** Voice to speak in: a file stem under voices/, or "default". Unknown falls back to
     *  "default", then the first voice. */
    std::string voice = "Easton";
    /** Language the text is normalized as before synthesis (numbers, amounts, dates, symbols and
     *  addresses spelled out as Gradium's runtime does): en, fr, de, es, pt, or "none" to pass the
     *  text as written. The language of the text, not of the voice. */
    std::string language = "en";
    /** Rewrite rules normalization runs: "default" (numbers, currency, dashed digits, emails, urls,
     *  abbreviations, elongations), "all" (also phones, times, dates), "none", or a comma-separated
     *  list of rule names. */
    std::string normalizationRules = "default";
    /** Sampling temperature; 0 is deterministic. */
    float temperature = 0.3f;
    /** Speaking-rate condition of the 40m checkpoints; negative is faster. */
    float durationDelta = 0.0f;
    /** Largest audio chunk handed over, in 80 ms frames (12: about 1 s). The first chunk is always
     *  6 frames so playback starts early. */
    int streamChunkFrames = 12;
    /** Longer text is spoken as groups of whole sentences of up to this many tokens; a sentence over
     *  200 tokens is cut at a clause. */
    int maxTokensPerChunk = 50;
    /** Silence after each completed synthesis sequence. */
    float silenceDurationSeconds = 0.3f;
};

/**
 * @brief Voice synthesis provider backed by Gradium's Phonon TTS (flow-matching transformer +
 *        Mimi decoder, 24 kHz mono), run by an in-tree C++ port of the inference on ggml
 *        (impl/), loading the Hugging Face checkpoint files as published.
 *
 * A QueuedVoiceSynthesisProvider: the base class queues the texts and cancels; this class loads
 * the model, synthesizes one text and delivers its audio straight from the engine's stream
 * thread (Delivery::Direct).
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
 * a 480 ms first chunk, then doubling up to the streamChunkFrames option.
 */
class PhononVoiceSynthesisProvider : public va::audio::QueuedVoiceSynthesisProvider {
public:
    explicit PhononVoiceSynthesisProvider(PhononVoiceSynthesisOptions options);
    ~PhononVoiceSynthesisProvider() override;

    // Non-copyable
    PhononVoiceSynthesisProvider(const PhononVoiceSynthesisProvider&)            = delete;
    PhononVoiceSynthesisProvider& operator=(const PhononVoiceSynthesisProvider&) = delete;

    // --- Switching at run time ---

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

private:
    class StreamListener;

    // Applied from the options by the constructor; each keeps the current value on a bad one.
    bool setNormalizationRules(const std::string& rules);
    void setTemperature(float temperature);
    void setStreamChunkFrames(int frames);
    void setMaxTokensPerChunk(int tokens);
    void setSilenceDuration(float seconds);

    std::string _modelDir;
    std::unique_ptr<phonon::PhononTTS> _engine;
    float _silenceDuration = 0.0f;

    int modelSampleRate() const { return 24000; }   // the model's output rate

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

    std::atomic<uint64_t> _activeStream{0};   // the engine stream being spoken, for onCancel()
    std::atomic<int>      _seed{42};          // a new sampling seed per chunk

    // Streaming resampler (24 kHz -> pipeline rate), carried across chunks so chunk boundaries
    // don't click. The engine's stream thread uses it, and the worker once a stream has ended.
    va::audio::StreamResampler _resampler;
    std::vector<float>         _resampleBuf;

    bool loadEngine() override;
    bool synthesizeOne(const std::string& text) override;
    void onCancel() override;
    void onSequenceCompleted() override;

    bool applyPendingVoice();
    std::string voicePath(const std::string& name) const;
    void scanVoices();
    void onModelAudio(const float* pcm, size_t n);
    void flushResampler();
};

} // namespace va::thirdparty::tts

#endif // PHONON_VOICE_SYNTHESIS_PROVIDER_HPP
