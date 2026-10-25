#pragma once
// phonon.h — Phonon TTS inference engine on ggml (C++ port of gradium-ai/xn-ptts).
// Loads the Hugging Face checkpoint as published: config.json, tokenizer.model,
// model.q8.gguf and voice .safetensors files.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gradium::tts {

    namespace text { class Tokenizer; }

    // ---------------------------------------------------------------------------
    // Configuration (mirrors config.json)
    // ---------------------------------------------------------------------------
    struct FlowLMConfig {
        int   d_model = 768;
        int   num_heads = 12;
        int   num_layers = 12;
        int   dim_feedforward = 3072;
        float max_period = 10000.0f;
        int   n_bins = 4000;
        int   lut_dim = 768;
        int   flow_dim = 512;
        int   flow_depth = 6;
        int   ldim = 32;
    };

    struct MimiConfig {
        int   channels = 1;
        int   sample_rate = 24000;
        float frame_rate = 12.5f;
        int   dimension = 512;
        int   quantizer_dim = 32;
        int   n_filters = 64;
        int   compress = 2;
        int   transformer_d_model = 512;
        int   transformer_num_heads = 8;
        int   transformer_num_layers = 2;
        float transformer_layer_scale = 0.01f;
        int   transformer_context = 250;
        float transformer_max_period = 10000.0f;
    };

    struct Config {
        FlowLMConfig flow_lm;
        MimiConfig   mimi;
        float temp = 0.3f;
        int   lsd_decode_steps = 1;
        float eos_threshold = -4.0f;
        // FFN activation: exact erf GELU for Phonon checkpoints ("gelu": "erf" or a
        // model_id in config.json), tanh approximation for Pocket TTS ones.
        bool  gelu_erf = false;
        // duration_delta continuous conditioner (Phonon 40m): sinusoidal embedding of
        // scale * delta, projected to d_model and summed into every generated step.
        float duration_delta_scale = 1000.0f;
        float duration_delta_max_period = 10000.0f;
    };

    // ---------------------------------------------------------------------------
    // Paths to model files
    // ---------------------------------------------------------------------------
    struct Paths {
        std::string model_gguf;   // model.q8.gguf
        std::string config_json;  // optional — defaults used if empty
        std::string tokenizer;    // tokenizer.model (SentencePiece)
        std::string voice;        // voice (.safetensors): "emb" [T, d_model] or "speaker_wavs" [1, C, T]
    };

    // ---------------------------------------------------------------------------
    // Per-call synthesis options
    // ---------------------------------------------------------------------------
    struct SynthOpts {
        float temperature = 0.3f;  // noise std = sqrt(temperature)
        float duration_delta = 0.0f;  // speaking-rate condition (checkpoints that have one)
        int   seed = 42;
        int   num_speakers = 1;     // LUT conditioner index (0, 1, or 2)
        int   max_frames = 0;     // 0 = auto-compute from token count
        float speed = 1.0f;  // linear resampler; >1 faster (pitch shifts)
        int   num_threads = 0;     // 0 = use hardware_concurrency from load time
        int   stream_chunk_frames = 0;     // >0 enables streaming via onAudioChunk
    };

    // ---------------------------------------------------------------------------
    // Statistics filled on onFinish
    // ---------------------------------------------------------------------------
    struct SynthesisStats {
        int   total_samples = 0;   // PCM samples produced (0 in streaming mode)
        float duration_sec = 0.f; // audio length in seconds
        float synthesis_ms = 0.f; // wall-clock time from onStart to onFinish
        float rtf = 0.f; // synthesis_ms / (duration_sec * 1000)
    };

    // ---------------------------------------------------------------------------
    // Stream identity
    // ---------------------------------------------------------------------------
    using StreamId = std::uint64_t;

    // ---------------------------------------------------------------------------
    // Observer interface — subclass and pass to synthesizeAsync()
    // ---------------------------------------------------------------------------
    class SynthesisListener {
        public:
        virtual ~SynthesisListener() = default;

        // Always the first call.
        virtual void onStart(StreamId id, const std::string& text) = 0;

        // Called N times when stream_chunk_frames > 0; default is a no-op.
        virtual void onAudioChunk(StreamId id, std::vector<float> chunk) { (void)id; (void)chunk; }

        // Always the last call, even on cancellation. pcm is empty in streaming mode.
        // Not called when synthesis fails; onError is called instead.
        virtual void onFinish(StreamId id, const std::vector<float>& pcm,
            const SynthesisStats& stats) = 0;

        // Synthesis threw (bad text, tokenizer failure, ...). Last call for the stream.
        virtual void onError(StreamId id, const std::string& message) { (void)id; (void)message; }
    };

    // ---------------------------------------------------------------------------
    // Main engine class
    // ---------------------------------------------------------------------------
    class PhononTTS {
        public:
        explicit PhononTTS(const Paths&, const Config & = Config{});
        ~PhononTTS();

        PhononTTS(const PhononTTS&) = delete;
        PhononTTS& operator=(const PhononTTS&) = delete;
        PhononTTS(PhononTTS&&)                 noexcept;
        PhononTTS& operator=(PhononTTS&&)      noexcept;

        // Blocking — runs on the calling thread; returns raw PCM (mono float32).
        std::vector<float> synthesize(const std::string& text,
            const SynthOpts& opts = SynthOpts{});

        // Non-blocking — returns immediately; callbacks arrive on a background thread.
        // The caller must keep |listener| alive until onFinish fires.
        StreamId synthesizeAsync(const std::string& text,
            const SynthOpts& opts,
            SynthesisListener& listener);

        // Set the cancelled flag; generation breaks at the next frame boundary.
        // onFinish still fires with whatever was collected before cancellation.
        void cancelStream(StreamId id);

        // Join the background thread and release its resources.
        void waitForStream(StreamId id);

        // Swap the voice (re-runs the voice prefix, ~100 ms). Must not be called while
        // a stream is in flight.
        void setVoice(const std::string& voice_path);

        int sampleRate() const noexcept;
        // Codec frames per second (12.5): the unit of text::frameBudget.
        double frameRate() const noexcept;

        // The checkpoint's tokenizer, for splitting long text with text::planChunks.
        // Thread-safe; lives as long as the engine.
        const text::Tokenizer& tokenizer() const noexcept;

        static bool   writeWav(const std::string& path,
            const std::vector<float>& pcm, int sample_rate);
        static Config parseConfig(const std::string& json_path);

        private:
        struct PhononTTSImpl;
        std::unique_ptr<PhononTTSImpl> _ttsImpl;
    };

} // namespace gradium::tts
