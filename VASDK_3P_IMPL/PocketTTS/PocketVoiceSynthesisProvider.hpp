#ifndef POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
#define POCKET_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>
#include <vector>

namespace va::thirdparty::tts {

class PocketTTSImpl;

/** Settings of PocketVoiceSynthesisProvider; every field but the model folder has a default. */
struct PocketVoiceSynthesisOptions {
    /** Folder with the ONNX models, tokenizer.model and voices.bin
     *  (VASDK_3P_IMPL/cmake/DownloadPocketTTSModels.cmake). */
    std::string modelDir;
    /** A voice in voices.bin; an unknown name falls back to the first voice, with a warning. */
    std::string voice = "jean";
    /** Flow-matching steps per latent frame, 1-10: higher sounds better and costs more per frame. */
    int lsdSteps = 4;
    /** Sampling temperature; the flow's noise is sqrt(temperature). */
    float temperature = 0.7f;
    /** Silence after each utterance. */
    float silenceDurationSeconds = 0.3f;
    /** Audio made before playback starts, against choppy starts on throttled phones
     *  (QueuedVoiceSynthesisProvider::setPreBufferedSamplesMS). */
    int prebufferMs = 250;
    /** Latent frames decoded to audio at a time: fewer start speech sooner, more decode faster. */
    int decoderBatchFrames = 12;
};

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
    explicit PocketVoiceSynthesisProvider(PocketVoiceSynthesisOptions options);
    ~PocketVoiceSynthesisProvider() override;

    // Non-copyable
    PocketVoiceSynthesisProvider(const PocketVoiceSynthesisProvider&)            = delete;
    PocketVoiceSynthesisProvider& operator=(const PocketVoiceSynthesisProvider&) = delete;

    /// Switch voices at run time. Falls back to the first available voice if not found.
    /// Thread-safe: applied before the next synthesizeOne() call.
    bool setVoice(const std::string& voiceName);
    std::string getVoice() const;

    /// Returns the list of available voice names parsed from voices.bin.
    std::vector<std::string> getAvailableVoices() const;

private:
    std::shared_ptr<PocketTTSImpl> _impl;

    int modelSampleRate() const { return 24000; }   // the model's output rate

    bool loadEngine() override;
    bool synthesizeOne(const std::string& text) override;

    // Text normalization: strip markdown, expand numbers, clean punctuation
    static std::string normalizeText(const std::string& text);
};

} // namespace va::thirdparty::tts

#endif // POCKET_VOICE_SYNTHESIS_PROVIDER_HPP
