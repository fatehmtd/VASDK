#ifndef SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP
#define SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>

class SupertonicInferenceImpl;

/**
 * @brief Voice synthesis provider backed by the Supertonic 3 local ONNX TTS engine.
 *
 * A QueuedVoiceSynthesisProvider: the base class queues the texts, cancels and delivers the
 * audio; this class loads the models and synthesizes one text.
 */
class SupertonicVoiceSynthesisProvider : public va::audio::QueuedVoiceSynthesisProvider {
public:
    /**
     * @param modelDir   Path to directory with ONNX models (tts.json, unicode_indexer.json, *.onnx)
     * @param stylePath  Path to a voice style JSON file (e.g. M1.json)
     * @param lang       Language code, any of the 31 in helper.cpp AVAILABLE_LANGS, e.g. en, ko, ja, de, fr (default: "en")
     */
    SupertonicVoiceSynthesisProvider(const std::string& modelDir,
                                     const std::string& stylePath,
                                     const std::string& lang = "en");
    ~SupertonicVoiceSynthesisProvider() override;

private:
    std::shared_ptr<SupertonicInferenceImpl> impl_;

    // Cached after initialize() - model sample rate is fixed for the session lifetime.
    int _modelSampleRate{0};

    bool loadEngine() override;
    /// Synthesizes one queued text, sentence by sentence. True on completion, false if cancelled.
    bool synthesizeOne(const std::string& text) override;

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