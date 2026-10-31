#ifndef MOSS_TTS_IMPL_HPP
#define MOSS_TTS_IMPL_HPP

#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

/**
 * @brief PIMPL wrapper for MOSS-TTS-Nano ONNX inference.
 *
 * Owns the ONNX Runtime sessions, SentencePiece tokenizer and the built-in voice prompts
 * parsed from browser_poc_manifest.json. Streams Float32 mono audio at 48 kHz via callback.
 *
 * Generation pipeline (per text input):
 *   1. Normalise text → split into chunks of ≤ maxChunkTokens SentencePiece tokens
 *   Per chunk:
 *     2. Build prompt rows [seq, 17]: user prefix text, voice prompt audio codes,
 *        "after reference" text, chunk tokens, assistant prefix
 *     3. moss_tts_prefill: rows → global_hidden + 12-layer KV cache
 *     4. Autoregressive loop (until should_continue == 0 or maxNewFrames):
 *          moss_tts_local_fixed_sampled_frame: last hidden → 16 audio codes (sampled in-graph)
 *          moss_tts_decode_step: frame row + KV cache → next hidden + KV cache
 *          Every few frames: moss_audio_tokenizer_decode_step (streaming codec) → audio → callback
 *     5. Inter-chunk pause / trailing silence → callback
 */
class MossTTSImpl {
public:
    struct Params {
        /// Built-in voice name from browser_poc_manifest.json (e.g. "Ava", "Adam", "Junhao").
        std::string voiceName    = "Ava";
        /// Hard cap on generated frames per chunk (12.5 frames = 1 s). 0 = manifest default (375).
        int   maxNewFrames       = 0;
        /// Max SentencePiece tokens per synthesis chunk (Python voice_clone_max_text_tokens).
        int   maxChunkTokens     = 75;
        /// Seconds of silence appended after each utterance.
        float silenceDuration    = 0.3f;
        /// Pause between the chunks of a long text: after a chunk of up to four words, and
        /// after longer ones (DEFAULT_VOICE_CLONE_INTER_CHUNK_PAUSE_SHORT/LONG_SECONDS).
        float pauseShortSeconds  = 0.40f;
        float pauseLongSeconds   = 0.24f;
        /// Full text normalization (MossTextNormalizer: cleanup + spelled-out numbers, dates,
        /// money, units...). When false, only markdown markers are stripped.
        bool  normalizeText      = true;
    };

    MossTTSImpl(const std::string& modelDir, const std::string& voiceName, int numThreads);
    ~MossTTSImpl() = default;

    MossTTSImpl(const MossTTSImpl&)            = delete;
    MossTTSImpl& operator=(const MossTTSImpl&) = delete;

    /// Load manifest, tokenizer and ONNX sessions. Returns false on failure.
    bool initialize();
    bool isInitialized() const { return initialized_; }

    int getSampleRate() const { return sampleRate_; }

    // --- Thread-safe parameter access ---
    void   setParams(const Params& p);
    Params getParams() const;

    /// Names of the built-in voices that ship prompt audio codes.
    std::vector<std::string> getVoiceNames() const;

    /// Calls onChunk with Float32 mono PCM chunks as they become available.
    /// Checks cancelFlag between steps - returns early if set.
    using AudioCallback = std::function<void(std::vector<float>)>;
    void synthesize(const std::string& text,
                    const std::atomic<bool>& cancelFlag,
                    const AudioCallback& onChunk);

    /// Strip markdown markers and collapse whitespace (used when normalizeText is off).
    static std::string stripMarkdown(const std::string& text);

private:
    struct Voice {
        std::string                       name;
        bool                              english = false;  // manifest group "English ..."
        std::vector<std::vector<int32_t>> promptAudioCodes; // [frames][nVq]
    };

    std::string modelDir_;
    int         numThreads_;
    bool        initialized_ = false;

    mutable std::mutex paramsMutex_;
    Params params_;

    // --- Manifest / model config ---
    int nVq_                    = 16;
    int rowWidth_               = 17;
    int codebookSize_           = 1024;
    int audioPadTokenId_        = 1024;
    int audioStartTokenId_      = 6;
    int audioEndTokenId_        = 7;
    int audioUserSlotTokenId_   = 8;
    int audioAssistantSlotTokenId_ = 9;
    int manifestMaxNewFrames_   = 375;
    int sampleRate_             = 48000;
    std::vector<int32_t> userPromptPrefixIds_;
    std::vector<int32_t> userPromptAfterReferenceIds_;
    std::vector<int32_t> assistantPromptPrefixIds_;
    std::vector<Voice>   voices_;

    // --- ONNX Runtime ---
    Ort::Env            env_{ORT_LOGGING_LEVEL_WARNING, "MossTTS"};
    Ort::SessionOptions sessionOpts_{};
    Ort::MemoryInfo     memInfo_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    std::unique_ptr<Ort::Session> prefillSession_;
    std::unique_ptr<Ort::Session> decodeSession_;
    std::unique_ptr<Ort::Session> localFrameSession_;
    std::unique_ptr<Ort::Session> codecStepSession_;

    std::vector<std::string> prefillInNames_,  prefillOutNames_;
    std::vector<std::string> decodeInNames_,   decodeOutNames_;
    std::vector<std::string> localInNames_,    localOutNames_;
    std::vector<std::string> codecInNames_,    codecOutNames_;

    // KV slot k: decode input index, and the matching "present_*" output index in each graph.
    std::vector<size_t> kvDecodeInIdx_;
    std::vector<size_t> kvPrefillOutIdx_;
    std::vector<size_t> kvDecodeOutIdx_;
    size_t prefillHiddenOutIdx_ = 0;
    size_t decodeHiddenOutIdx_  = 0;

    // Streaming codec state: one entry per codec input other than audio_codes/audio_code_lengths.
    struct CodecStateSpec {
        std::string          inName;
        std::string          outName;
        size_t               inIdx  = 0;
        size_t               outIdx = 0;
        std::vector<int64_t> shape;
        bool                 isFloat = false;
        int32_t              fillInt = 0;   // initial value for int32 states (-1 for positions)
    };
    std::vector<CodecStateSpec> codecStateSpecs_;
    std::vector<Ort::Value>     codecState_;

    std::mt19937 rng_;

    // --- SentencePiece tokenizer ---
    std::unique_ptr<sentencepiece::SentencePieceProcessor> tokenizer_;

    bool loadManifest(std::string& ttsDir, std::string& codecDir, std::string& codecStepFile,
                      std::string& prefillFile, std::string& decodeFile, std::string& localFrameFile);
    bool loadModels(const std::string& ttsDir, const std::string& codecDir,
                    const std::string& prefillFile, const std::string& decodeFile,
                    const std::string& localFrameFile, const std::string& codecStepFile);
    bool resolveIoBindings();

    std::vector<int32_t> tokenize(const std::string& text) const;
    int countTokens(const std::string& text) const;
    std::vector<std::string> splitIntoChunks(const std::string& text, int maxTokens) const;
    std::vector<std::string> splitByTokenBudget(const std::u32string& text, int maxTokens) const;

    const Voice* findVoice(const std::string& name) const;

    // Playback lead tracking for one synthesize() call: how far emitted audio runs ahead of
    // wall-clock time since the first emission. Drives the codec decode batch size.
    struct StreamClock {
        bool   started         = false;
        double startSeconds    = 0.0;
        size_t emittedSamples  = 0;
    };

    /// Generate audio for one chunk; streams audio through onChunk. Returns false if cancelled.
    bool synthesizeChunk(const std::vector<int32_t>& textIds, const Voice& voice, int maxFrames,
                         StreamClock& clock, const std::atomic<bool>& cancelFlag,
                         const AudioCallback& onChunk);
    void emit(std::vector<float> pcm, StreamClock& clock, const AudioCallback& onChunk) const;

    void resetCodecState();
    /// Run the streaming codec on frames [nFrames][nVq]; returns mono samples.
    std::vector<float> runCodecFrames(const std::vector<int32_t>& frames, int nFrames);
};

#endif // MOSS_TTS_IMPL_HPP
