#ifndef MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
#define MOSS_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>
#include <vector>

namespace va::thirdparty {

class MossTTSImpl;

/** Settings of MossVoiceSynthesisProvider; every field but the model folder has a default. */
struct MossVoiceSynthesisOptions {
    /** Folder containing MOSS-TTS-Nano-100M-ONNX/ and MOSS-Audio-Tokenizer-Nano-ONNX/, or the TTS
     *  folder itself (VASDK_3P_IMPL/cmake/DownloadMossTTSModels.cmake). */
    std::string modelDir;
    /** Built-in voice, e.g. "Ava", "Bella", "Adam", "Nathan", "Junhao", "Xiaoyu", "Soyo";
     *  unknown falls back to the first voice, with a warning. */
    std::string voice = "Ava";
    /** ONNX Runtime intra-op threads per session. */
    int threads = 4;
    /** Silence after each utterance. */
    float silenceDurationSeconds = 0.3f;
    /** Cap on generated frames per text chunk, 12.5 frames a second; 0 is the manifest's 375. */
    int maxNewFrames = 0;
    /** SentencePiece tokens per synthesis chunk; longer text splits at sentence or clause ends. */
    int maxChunkTokens = 75;
    /** Spell numbers, dates, times, money, units and abbreviations out before synthesis (English,
     *  Chinese or Japanese, by script). Off: only markdown markers are stripped. */
    bool textNormalization = true;
    /** Audio made before playback starts, against generation jitter on slow CPUs; 0 streams at once
     *  (QueuedVoiceSynthesisProvider::setPreBufferedSamplesMS). */
    int prebufferMs = 0;
};

/**
 * @brief Voice synthesis provider backed by MOSS-TTS-Nano (OpenMOSS, ~100M params) - an
 *        autoregressive multilingual (zh / en / ja, ...) ONNX TTS producing 48 kHz audio.
 *
 * A QueuedVoiceSynthesisProvider: the base class queues the texts, cancels and delivers the
 * audio; this class loads the models and synthesizes one text. Audio is streamed: frames are
 * decoded by the streaming audio-tokenizer graph as they are generated, then resampled to the
 * pipeline rate.
 *
 * Model directory layout (as downloaded by DownloadMossTTSModels.cmake):
 *   MOSS-TTS-Nano-100M-ONNX/
 *     browser_poc_manifest.json   (prompt templates + built-in voice prompt codes)
 *     tts_browser_onnx_meta.json
 *     tokenizer.model             (SentencePiece)
 *     moss_tts_prefill.onnx, moss_tts_decode_step.onnx, moss_tts_global_shared.data
 *     moss_tts_local_fixed_sampled_frame.onnx, moss_tts_local_shared.data
 *   MOSS-Audio-Tokenizer-Nano-ONNX/
 *     codec_browser_onnx_meta.json
 *     moss_audio_tokenizer_decode_step.onnx, moss_audio_tokenizer_decode_shared.data
 */
class MossVoiceSynthesisProvider : public va::audio::QueuedVoiceSynthesisProvider {
public:
    explicit MossVoiceSynthesisProvider(MossVoiceSynthesisOptions options);
    ~MossVoiceSynthesisProvider() override;

    MossVoiceSynthesisProvider(const MossVoiceSynthesisProvider&)            = delete;
    MossVoiceSynthesisProvider& operator=(const MossVoiceSynthesisProvider&) = delete;

    // --- Switching at run time ---

    /// Select a built-in voice by name. Applied before the next utterance.
    /// Returns false (and keeps the name, falling back at synthesis time) if the voice is
    /// unknown; voices are only known after startVoiceSynthesisProvider().
    bool setVoice(const std::string& voiceName);
    std::string getVoice() const;

    /// Built-in voice names from the manifest (empty until the provider is started).
    std::vector<std::string> getAvailableVoices() const;

private:
    std::shared_ptr<MossTTSImpl> _impl;
    int  _modelSampleRate = 0;

    /// Joins the small per-frame chunks without edge artifacts. The worker thread's only.
    va::audio::StreamResampler _resampler;

    bool loadEngine() override;
    bool synthesizeOne(const std::string& text) override;
};

} // namespace va::thirdparty

#endif // MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
