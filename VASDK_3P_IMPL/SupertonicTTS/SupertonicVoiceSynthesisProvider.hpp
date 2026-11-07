#ifndef SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP
#define SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>

namespace va::thirdparty::tts {

class SupertonicInferenceImpl;

/** Settings of SupertonicVoiceSynthesisProvider; every field but the two paths has a default. */
struct SupertonicVoiceSynthesisOptions {
    /** Folder with the ONNX models, tts.json and unicode_indexer.json: <supertonic3>/onnx
     *  (VASDK_3P_IMPL/cmake/DownloadSupertonicModels.cmake). */
    std::string onnxDir;
    /** Voice style JSON, e.g. <supertonic3>/voice_styles/M1.json. */
    std::string voiceStylePath;
    /** Any of the 31 codes in the upstream helper.cpp AVAILABLE_LANGS, e.g. en, ko, ja, de, fr. */
    std::string language = "en";
    /** Diffusion steps: synthesis time grows about linearly with them. 5 is fast but rougher,
     *  10 is a good balance for a voice assistant, 15+ adds little. */
    int totalSteps = 10;
    /** Speaking speed: 1.0 is natural, 1.05 is 5% faster. */
    float speed = 1.05f;
    /** Silence after each synthesized chunk; below about 0.1 s chunks join with hard cuts. */
    float silenceDurationSeconds = 0.3f;
    /** When above 0, each chunk is cut to sample_rate * predicted duration * (1 + trimMargin), removing
     *  the vocoder's silent tail. Off by default: the duration predictor can underestimate and clip
     *  the last phoneme. */
    float trimMargin = 0.0f;
};

/**
 * @brief Voice synthesis provider backed by the Supertonic 3 local ONNX TTS engine.
 *
 * A QueuedVoiceSynthesisProvider: the base class queues the texts, cancels and delivers the
 * audio; this class loads the models and synthesizes one text.
 */
class SupertonicVoiceSynthesisProvider : public va::audio::QueuedVoiceSynthesisProvider {
public:
    explicit SupertonicVoiceSynthesisProvider(SupertonicVoiceSynthesisOptions options);
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

};

} // namespace va::thirdparty::tts

#endif // SUPERTONIC_VOICE_SYNTHESIS_PROVIDER_HPP