#ifndef SUPERTONIC_INFERENCE_IMPL_HPP
#define SUPERTONIC_INFERENCE_IMPL_HPP

#include "helper.h"
#include <string>
#include <vector>
#include <mutex>

/**
 * @brief PIMPL wrapper around supertonic's ONNX-based TTS engine.
 *
 * Owns the ONNX Runtime environment, loaded models, voice style, and
 * provides a thread-safe synthesize() call that returns raw float PCM.
 */
class SupertonicInferenceImpl {
public:
    struct SynthesisParams {
        /// Number of diffusion/flow-matching steps. Higher = better quality, slower inference.
        /// 5 is fast but noticeably lower quality; 10 is a good balance for voice assistants;
        /// 15+ gives marginal improvement over 10 for most voice styles.
        ///   5 steps:  ~0.8–1.0 s/chunk  - acceptable for constrained devices
        ///  10 steps:  ~1.5–2.0 s/chunk  - good conversational quality (default)
        ///  15 steps:  ~2.2–3.0 s/chunk  - marginal improvement over 10
        int   totalSteps      = 10;
        /// Speaking speed multiplier. 1.0 = natural rate; 1.05 = 5 % faster.
        float speed           = 1.05f;
        /// Seconds of silence appended after each synthesized utterance to separate chunks cleanly.
        /// The official supertonic default is 0.3 s.  Values below ~0.1 s produce abrupt
        /// hard-cut transitions that can sound like stuttering.
        float silenceDuration = 0.3f;
        /// When > 0, the vocoder output is trimmed to
        ///   sample_rate * duration[0] * (1 + trimMargin)
        /// removing the silent tail produced by the vocoder on padded latent frames.
        /// Default 0 (disabled) because the duration predictor can underestimate,
        /// causing word clipping.  Enable only after verifying the predictor is
        /// accurate enough on your voice style.
        float trimMargin      = 0.0f;
    };

    /**
     * @param onnxDir   Path to directory containing ONNX models + tts.json + unicode_indexer.json
     * @param stylePath Path to a voice style JSON file (e.g. M1.json)
     * @param lang      Language code: en, ko, es, pt, fr
     */
    SupertonicInferenceImpl(const std::string& onnxDir,
                            const std::string& stylePath,
                            const std::string& lang = "en");
    ~SupertonicInferenceImpl() = default;

    /// Load ONNX models and voice style. Returns false on failure.
    bool initialize();

    /// Run a short dummy inference to force NNAPI graph compilation, ORT arena
    /// pre-allocation, and DSP power-state transition.  Called automatically at
    /// the end of initialize() so the first real synthesis call doesn't pay the
    /// cold-start penalty.  Uses fewer diffusion steps on Android to keep init fast.
    void warmUp();

    struct SynthesisOutput {
        std::vector<float> wav;
        float durationSeconds = 0.0f; ///< Duration predictor estimate (after speed factor)
    };

    /// Run TTS inference. Returns float PCM at getSampleRate(), trimmed to durationSeconds.
    SynthesisOutput synthesize(const std::string& text);

    int getSampleRate() const;

    /// Thread-safe: waits for any in-flight synthesis to finish before applying.
    void setParams(const SynthesisParams& params);
    SynthesisParams getParams() const;

    /// Thread-safe: waits for any in-flight synthesis to finish before applying.
    void setLanguage(const std::string& lang);
    const std::string& getLanguage() const { return lang_; }

private:
    std::string onnxDir_;
    std::string stylePath_;
    std::string lang_;
    SynthesisParams params_;

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "SupertonicTTS"};
    std::unique_ptr<TextToSpeech> tts_;
    std::unique_ptr<Style> style_;
    Ort::MemoryInfo memoryInfo_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    mutable std::mutex mutex_; // guard inference and config (ONNX sessions are not thread-safe)
};

#endif // SUPERTONIC_INFERENCE_IMPL_HPP