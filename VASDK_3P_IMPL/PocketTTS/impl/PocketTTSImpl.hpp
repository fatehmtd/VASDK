#ifndef POCKET_TTS_IMPL_HPP
#define POCKET_TTS_IMPL_HPP

#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief PIMPL wrapper for Pocket TTS ONNX inference.
 *
 * Owns the ONNX Runtime sessions, SentencePiece tokenizer, voice embeddings,
 * and KV-cache state buffers. Provides a single thread-safe synthesize() call
 * that streams Float32 audio chunks at 24 kHz via callback.
 *
 * Generation pipeline (per text input):
 *   1. Normalise text → tokenise via SentencePiece → chunk to ≤50 tokens
 *   Per chunk:
 *     2. text_conditioner: token_ids → text_emb [1, N, 1024]
 *     3. flow_lm_main (text conditioning phase):
 *          seq=[1,0,32], text_embeddings=text_emb → KV states updated
 *     4. flow_lm_main (voice conditioning phase):
 *          seq=[1,0,32], text_embeddings=voice_emb → KV states updated
 *     5. Autoregressive loop (until EOS or MAX_FRAMES):
 *          flow_lm_main: seq=[prev_latent], text_embeddings=[1,0,1024]
 *                        → conditioning c [1,1,32], eosLogit, updated states
 *          ODE solver (lsdSteps × flow_lm_flow):
 *                        → latent [32]
 *          Every decoderBatchFrames latents: mimi_decoder → audio chunk → callback
 *     6. Flush remaining latents → callback
 *     7. Silence padding → callback
 */
class PocketTTSImpl {
public:
    struct Params {
        /// ODE solver steps per latent frame (1–10). Higher = better quality.
        int   lsdSteps           = POCKET_TTS_DEFAULT_LSD_STEPS;
        /// Sampling temperature; ODE noise std is sqrt(temperature).
        /// Matches Python DEFAULT_TEMPERATURE = 0.7.
        float temperature        = 0.7f;
        /// Seconds of silence appended after each utterance.
        float silenceDuration    = 0.3f;
        /// Number of latent frames batched per mimi_decoder call.
        /// The streaming path uses JS constants (3 first / 12 normal); this
        /// field is kept for external API compatibility only.
        int   decoderBatchFrames = 12;
        /// Active voice name (looked up in the loaded voices on each synthesize call).
        std::string voiceName    = "cosette";
    };

    // Raw tensor record from voices.bin (PTVB1 format).
    struct TensorRecord {
        uint8_t              dtype = 0; // 0=float32, 1=int64, 2=bool/uint8
        std::vector<uint32_t> shape;
        std::vector<uint8_t>  bytes; // raw little-endian bytes
    };

    // A voice is a dict of pre-computed flow LM KV states keyed by
    // "{module}/{key}", e.g. "transformer.layers.0.self_attn/cache".
    struct VoiceEntry {
        std::string name;
        std::unordered_map<std::string, TensorRecord> tensors;
    };

    PocketTTSImpl(const std::string& modelDir, const std::string& voiceName);
    ~PocketTTSImpl() = default;

    PocketTTSImpl(const PocketTTSImpl&)            = delete;
    PocketTTSImpl& operator=(const PocketTTSImpl&) = delete;

    /// Load models, tokenizer, and voices. Returns false on failure.
    bool initialize();
    bool isInitialized() const { return initialized_; }

    int getSampleRate() const { return 24000; }

    // --- Thread-safe parameter access ---
    void   setParams(const Params& p);
    Params getParams() const;

    /// Returns all loaded voice names.
    std::vector<std::string> getVoiceNames() const;

    // --- Core synthesis ---
    // Calls onChunk with Float32 PCM chunks as they become available.
    // Checks cancelFlag between steps - returns early if set.
    using AudioCallback = std::function<void(std::vector<float>)>;
    void synthesize(const std::string& text,
                    const std::atomic<bool>& cancelFlag,
                    const AudioCallback& onChunk);

private:
    std::string modelDir_;
    bool        initialized_ = false;

    mutable std::mutex paramsMutex_;
    Params params_;

    // --- ONNX Runtime ---
    Ort::Env            env_{ORT_LOGGING_LEVEL_WARNING, "PocketTTS"};
    Ort::SessionOptions sessionOpts_{};
    Ort::MemoryInfo     memInfo_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    std::unique_ptr<Ort::Session> textCondSession_;
    std::unique_ptr<Ort::Session> flowLmMainSession_;
    std::unique_ptr<Ort::Session> flowLmFlowSession_;
    std::unique_ptr<Ort::Session> mimiDecSession_;

    // Cached input/output name strings (allocated once at initialize() time)
    std::vector<std::string> flowLmMainInNames_;
    std::vector<std::string> flowLmMainOutNames_;
    std::vector<std::string> flowLmFlowInNames_;
    std::vector<std::string> flowLmFlowOutNames_;
    std::vector<std::string> mimiDecInNames_;
    std::vector<std::string> mimiDecOutNames_;
    std::vector<std::string> textCondInNames_;
    std::vector<std::string> textCondOutNames_;

    // --- SentencePiece tokenizer ---
    std::unique_ptr<sentencepiece::SentencePieceProcessor> tokenizer_;

    // --- Voice embeddings (from voices.bin) ---
    std::vector<VoiceEntry> voices_;

    // --- Flow LM state storage (re-used across AR steps within one chunk) ---
    // 18 states: {0,3,6,9,12,15} → KV cache [2,1,1000,16,64] float32
    //            {1,4,7,10,13,16} → empty [0] float32
    //            {2,5,8,11,14,17} → step counter [1] int64
    static constexpr int    kNumFlowStates = 18;
    static constexpr size_t kKVElems       = 2ULL * 1 * 1000 * 16 * 64; // 2,048,000
    static constexpr int64_t kKVShape[]    = {2, 1, 1000, 16, 64};

    enum class FlowStateType { KV_FLOAT, EMPTY_FLOAT, STEP_INT64 };
    FlowStateType flowStateType(int slot) const {
        int r = slot % 3;
        if (r == 0) return FlowStateType::KV_FLOAT;
        if (r == 1) return FlowStateType::EMPTY_FLOAT;
        return FlowStateType::STEP_INT64;
    }

    // 6 KV-cache buffers (for slots 0,3,6,9,12,15)
    std::array<std::vector<float>,   6> kvBufs_;
    // 6 step-counter buffers (for slots 2,5,8,11,14,17)
    std::array<std::vector<int64_t>, 6> stepBufs_;
    // Empty buffer shared by all EMPTY_FLOAT states
    std::vector<float> emptyFloatBuf_;

    // Voice-conditioning cache: built once per voice, reused for every text chunk.
    // JS equivalent: voiceConditioningCache / buildVoiceConditionedState.
    struct VoiceCondState {
        std::string                         name;      // voice name (empty = uncached)
        std::array<std::vector<float>,   6> kvBufs;    // KV-cache snapshot
        std::array<std::vector<int64_t>, 6> stepBufs;  // step-counter snapshot
    };
    VoiceCondState voiceCondState_;

    // --- MIMI decoder state storage (56 states, shapes introspected at init) ---
    static constexpr int kNumMimiDecStates = 56;
    enum class MimiStateType { FLOAT, BOOL, INT64 };
    std::vector<std::vector<float>>          mimiDecStateBufs_;      // FLOAT states
    std::vector<std::vector<bool>>           mimiDecStateBoolBufs_;  // BOOL  states
    std::vector<std::vector<int64_t>>        mimiDecStateInt64Bufs_; // INT64 states
    std::vector<std::vector<int64_t>>        mimiDecStateShapes_;
    std::vector<MimiStateType>               mimiDecStateType_;

    // --- Random number generator (for ODE noise initialisation) ---
    mutable std::mt19937                           rng_;
    mutable std::normal_distribution<float>        normalDist_{0.0f, 1.0f};

    // --- Internal helpers ---
    bool loadModels();
    bool loadTokenizer();
    bool loadVoices();
    void introspectMimiDecStates();
    // (fetchNames is a file-local helper in PocketTTSImpl.cpp)

    // Tokenise text; returns int64 token IDs.
    std::vector<int64_t> tokenize(const std::string& text) const;

    // Convert token IDs back to a text string (SentencePiece Decode).
    std::string detokenize(const std::vector<int64_t>& ids) const;

    // Split normalised text at .!? sentence boundaries, merge adjacent sentences
    // into chunks whose token count does not exceed maxTokens (JS: splitIntoBestSentences).
    std::vector<std::string> chunkBySentences(const std::string& text, int maxTokens) const;

    // Rebuild voice-conditioning cache when the voice changes, then restore
    // kvBufs_ / stepBufs_ from the cache (called once per text chunk).
    // JS equivalent: ensureVoiceConditioningCached + cloneFlowState.
    void refreshVoiceState(const VoiceEntry* voice);

    // Reset all flow LM states to zero (call before each new conditioning pass).
    void resetFlowLMStates();

    // Reset MIMI decoder states to zero (call once before the first AR loop).
    void resetMimiDecStates();

    // --- ONNX session helpers ---
    // text_conditioner: [1,N] int64 → [1,N,1024] float
    std::vector<float> runTextConditioner(const std::vector<int64_t>& tokenIds);

    // flow_lm_main: conditioning phase.
    //   seqData: float buffer for the sequence input (may be empty if seqLen==0)
    //   seqLen:  number of frames in sequence (0 for conditioning phases)
    //   textEmb: text/voice embedding buffer [1, embN, embDim] (empty if embN==0)
    //   embN:    number of embedding frames (0 for AR phase)
    //   embDim:  embedding width (1024 for text; use voice->embedDim for voice)
    // Returns the conditioning vector c and eosLogit scalar.
    struct FlowLMMainResult {
        std::vector<float> c;        // conditioning (cDim elements)
        float              eosLogit; // scalar
    };
    FlowLMMainResult runFlowLMMain(const float* seqData, int64_t seqLen,
                                   const float* textEmb,  int64_t embN,
                                   int64_t embDim = 1024);

    // flow_lm_flow: ODE velocity step.
    //   c: conditioning [1,1,32]
    //   s, t: time scalars (s = j/steps, t = (j+1)/steps)
    //   x: latent [32] (in-place updated by ODE integration)
    // Returns flow_dir [32].
    std::vector<float> runFlowLMFlow(const std::vector<float>& c, float s, float t,
                                     const std::vector<float>& x);

    // mimi_decoder: [1, numFrames, 32] → audio samples float.
    std::vector<float> runMimiDecoder(const std::vector<float>& latents, int64_t numFrames);

    // Copy output state tensors from an ORT Run back into the flow LM state buffers.
    void updateFlowLMStates(const std::vector<Ort::Value>& outputs, size_t stateStartIdx);

    // Build the flow LM state input tensors (appended in slot order into dst).
    void appendFlowLMStateInputs(std::vector<Ort::Value>& dst);

public:
    // Text normalisation helpers (shared with Supertonic logic)
    static std::string stripMarkdown(const std::string& text);
    static std::string expandNumbers(const std::string& text);
    static std::string normalizeText(const std::string& text);
};

#endif // POCKET_TTS_IMPL_HPP
