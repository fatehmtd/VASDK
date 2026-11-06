#ifndef POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
#define POCKET_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>
#include <vector>

class PocketTTSImpl;

/**
 * @brief Voice synthesis provider backed by Pocket TTS - a 5-model ONNX pipeline
 *        implementing autoregressive flow-matching TTS at 24 kHz.
 *
 * A QueuedVoiceSynthesisProvider: the base class queues the texts, cancels and delivers the
 * audio; this class loads the models and synthesizes one text.
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
class PocketVoiceSynthesisProvider : public va::audio::QueuedVoiceSynthesisProvider {
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

    // setPreBufferedSamplesMS() (the base class): default 1000 ms here, against choppy starts on
    // throttled phones.

private:
    std::shared_ptr<PocketTTSImpl> _impl;

    int modelSampleRate() const { return 24000; }   // the model's output rate

    bool loadEngine() override;
    bool synthesizeOne(const std::string& text) override;

    // Text normalization: strip markdown, expand numbers, clean punctuation
    static std::string normalizeText(const std::string& text);
};

#endif // POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
