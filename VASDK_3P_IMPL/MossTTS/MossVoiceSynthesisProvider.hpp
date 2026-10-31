#ifndef MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
#define MOSS_VOICE_SYNTHESIS_PROVIDER_HPP

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class MossTTSImpl;

/**
 * @brief Voice synthesis provider backed by MOSS-TTS-Nano (OpenMOSS, ~100M params) - an
 *        autoregressive multilingual (zh / en / ja, ...) ONNX TTS producing 48 kHz audio.
 *
 * Implements GenericVoiceSynthesisProvider so it can be plugged directly into
 * TTSFrameProcessor in the VASDK pipeline. Audio is streamed: frames are decoded by the
 * streaming audio-tokenizer graph as they are generated, then resampled to the pipeline rate.
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
class MossVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
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

    /// Audio (ms) to accumulate before the first chunk reaches the listener - absorbs
    /// generation jitter on slow CPUs. Default 0 (stream immediately).
    void setPreBufferedSamplesMS(int ms) { _preBufferedSamplesMS = std::max(0, ms); }
    int  getPreBufferedSamplesMS() const { return _preBufferedSamplesMS; }

private:
    std::shared_ptr<MossTTSImpl> _impl;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    bool _running = false;
    int  _modelSampleRate = 0;
    int  _preBufferedSamplesMS = 0;

    // Synthesis worker thread (same pattern as PocketVoiceSynthesisProvider)
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

    // Pre-allocated format-conversion buffers (owned by _deliveryThread)
    std::vector<int16_t> _int16Buf;
    std::vector<int32_t> _int32Buf;
    std::vector<uint8_t> _uint8Buf;

    /// Streaming windowed-sinc resampler: keeps input history across chunks so the small
    /// per-frame chunks join without edge artifacts. Owned by the synthesis thread.
    class StreamResampler {
    public:
        void reset(int srcRate, int dstRate);
        void process(const float* in, size_t n, std::vector<float>& out);
        void flush(std::vector<float>& out);
        bool passthrough() const { return _src == _dst || _src <= 0 || _dst <= 0; }
    private:
        int     _src = 0, _dst = 0, _kernelR = 0;
        int     _lanczosA = 3;       // Lanczos window size (lobes)
        double  _step = 1.0, _filterScale = 1.0;
        std::vector<float> _buf;     // input samples from absolute index _bufStart
        int64_t _bufStart = 0;
        int64_t _nextOut  = 0;
        void produce(std::vector<float>& out, int64_t inputEnd);
    };
    StreamResampler _resampler;

    void workerLoop();
    void deliveryLoop();
    bool synthesizeOne(const std::string& text);
    void handoffAudio(std::vector<float> wav);
    void flushDeliverySlot();
    void deliverAudio(const std::vector<float>& pcm);
};

#endif // MOSS_VOICE_SYNTHESIS_PROVIDER_HPP
