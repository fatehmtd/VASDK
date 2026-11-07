#ifndef MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
#define MOSS_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <memory>
#include <string>
#include <vector>

class MossTTSImpl;

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
    /**
     * @param modelDir    Directory containing MOSS-TTS-Nano-100M-ONNX/ and
     *                    MOSS-Audio-Tokenizer-Nano-ONNX/ (or the TTS directory itself).
     * @param voiceName   Built-in voice (e.g. "Ava", "Bella", "Adam", "Nathan", "Junhao", "Xiaoyu",
     *                    "Soyo"). Falls back to the first voice if not found.
     * @param numThreads  ONNX Runtime intra-op threads per session.
     */
    explicit MossVoiceSynthesisProvider(const std::string& modelDir,
                                        const std::string& voiceName = "Ava",
                                        int numThreads = 4);
    ~MossVoiceSynthesisProvider() override;

    MossVoiceSynthesisProvider(const MossVoiceSynthesisProvider&)            = delete;
    MossVoiceSynthesisProvider& operator=(const MossVoiceSynthesisProvider&) = delete;

    // --- Synthesis knobs ---

    /// Select a built-in voice by name. Applied before the next utterance.
    /// Returns false (and keeps the name, falling back at synthesis time) if the voice is
    /// unknown; voices are only known after startVoiceSynthesisProvider().
    bool setVoice(const std::string& voiceName);
    std::string getVoice() const;

    /// Built-in voice names from the manifest (empty until the provider is started).
    std::vector<std::string> getAvailableVoices() const;

    /// Seconds of silence appended after each utterance (default 0.3).
    void  setSilenceDuration(float seconds);
    float getSilenceDuration() const;

    /// Cap on generated frames per text chunk, 12.5 frames per second (default 0 = manifest's 375).
    void setMaxNewFrames(int frames);
    int  getMaxNewFrames() const;

    /// Max SentencePiece tokens per synthesis chunk; longer text is split at sentence /
    /// clause boundaries (default 75, as upstream).
    void setMaxChunkTokens(int tokens);
    int  getMaxChunkTokens() const;

    /// Spell numbers, dates, times, money, units and abbreviations out before synthesis, in
    /// English, Chinese or Japanese by script (default true). When false, only markdown
    /// markers are stripped and the model reads the text as written.
    void setTextNormalization(bool enabled);
    bool getTextNormalization() const;

    // setPreBufferedSamplesMS() (the base class): absorbs generation jitter on slow CPUs.
    // Default 0 (stream immediately).

private:
    std::shared_ptr<MossTTSImpl> _impl;
    int  _modelSampleRate = 0;

    /// Joins the small per-frame chunks without edge artifacts. The worker thread's only.
    va::audio::StreamResampler _resampler;

    bool loadEngine() override;
    bool synthesizeOne(const std::string& text) override;
};

#endif // MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
