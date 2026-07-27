#include "SupertonicInferenceImpl.hpp"
#include <spdlog/spdlog.h>
#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif

SupertonicInferenceImpl::SupertonicInferenceImpl(const std::string& onnxDir,
                                                 const std::string& stylePath,
                                                 const std::string& lang)
    : onnxDir_(onnxDir), stylePath_(stylePath), lang_(lang) {}

bool SupertonicInferenceImpl::initialize() {
    try {
        Ort::SessionOptions opts;
        // 4 threads: vocoder is the bottleneck and benefits from more cores;
        // 2 was underutilizing big cores on mobile SoCs.
        opts.SetIntraOpNumThreads(4);
        opts.SetInterOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef VASDK_USE_NNAPI
        // NNAPI_FLAG_USE_FP16: offload vocoder + flow-matching models to mobile GPU/DSP as FP16.
        // NNAPI_FLAG_USE_BURST_COMPUTE: keeps DSP in high-perf state between the rapid
        //   successive calls inside the flow-matching diffusion loop, eliminating power-state
        //   transition overhead (~5–20 ms/step).
        // CPU fallback remains active (no NNAPI_FLAG_CPU_DISABLED) for unsupported ops.
        auto ortStatus= OrtSessionOptionsAppendExecutionProvider_Nnapi(opts, NNAPI_FLAG_USE_FP16);
#endif
        auto cfgs      = loadCfgs(onnxDir_);
        auto models    = loadOnnxAll(env_, onnxDir_, opts);
        auto text_proc = loadTextProcessor(onnxDir_);
        tts_ = std::make_unique<TextToSpeech>(cfgs,
            text_proc.get(), models.dp.get(), models.text_enc.get(),
            models.vector_est.get(), models.vocoder.get());
        // Keep models and text_proc alive for the lifetime of tts_
        static OnnxModels static_models;
        static std::unique_ptr<UnicodeProcessor> static_text_proc;
        static_models    = std::move(models);
        static_text_proc = std::move(text_proc);
        auto loadedStyle = loadVoiceStyle({stylePath_}, /*verbose=*/false);
        style_ = std::make_unique<Style>(std::move(loadedStyle));
        spdlog::info("SupertonicTTS initialized (sample_rate={}, onnx_dir={}, style={})",
                     tts_->getSampleRate(), onnxDir_, stylePath_);
        warmUp();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("SupertonicTTS initialization failed: {}", e.what());
        return false;
    }
}

SupertonicInferenceImpl::SynthesisOutput SupertonicInferenceImpl::synthesize(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    clearTensorBuffers();
    // Use batch() instead of call() to bypass supertonic's buggy chunkText()
    // which drops words. Our caller already splits text into sentences.
    auto result = tts_->batch(memoryInfo_, {text}, {lang_}, *style_,
                              params_.totalSteps, params_.speed);

    int sampleRate = tts_->getSampleRate();

    // The vocoder pads its output to a multiple of base_chunk_size * chunk_compress_factor.
    // The padding is at most (base_chunk_size * chunk_compress_factor - 1) samples (~21 ms
    // at 24 kHz) of near-silence produced by the vocoder on zero-masked latent frames.
    // That tiny tail is harmless, so trimming is disabled by default.
    //
    // The duration predictor can underestimate the actual speech length (especially on
    // inputs with long words or sentence-final consonant clusters), so trimming to
    // sample_rate * duration[0] risks clipping the last phoneme(s).  Only enable
    // trimming (via trimMargin > 0) if you specifically need to remove the silent tail
    // AND you have verified the predictor is accurate enough on your voice style.
    SynthesisOutput out;
    out.durationSeconds = (!result.duration.empty() && sampleRate > 0)
                          ? result.duration[0] : 0.0f;

    if (params_.trimMargin > 0.0f && sampleRate > 0 && !result.duration.empty()) {
        int wavLen = static_cast<int>(sampleRate * result.duration[0] * (1.0f + params_.trimMargin));
        wavLen = std::max(0, std::min(wavLen, static_cast<int>(result.wav.size())));
        result.wav.resize(wavLen);
    }

    // Inter-chunk silence is now appended by the provider after resampling, so it
    // never passes through the Lanczos resampler. See SupertonicVoiceSynthesisProvider.

    out.wav = std::move(result.wav);
    return out;
}

void SupertonicInferenceImpl::warmUp() {
    // On Android the NNAPI driver JIT-compiles the ONNX graph on the very first
    // inference call (can take 1–5 s depending on SoC).  Running a minimal dummy
    // inference here, before any real speech request arrives, moves that compilation
    // cost to initialization time so audio delivery is never blocked by it.
    //
    // We also trigger ORT's arena pre-allocation and DSP power-state transition here,
    // which eliminates the per-call overhead visible as a resource spike on the first
    // real synthesis request.
#ifdef __ANDROID__
    // Fewer steps on Android: we only need to compile the NNAPI graph, not quality audio.
    constexpr int kWarmUpSteps = 4;
#else
    // On other platforms a cheap run still pre-allocates the ORT memory arena.
    constexpr int kWarmUpSteps = 2;
#endif
    try {
        spdlog::debug("SupertonicTTS: warm-up inference ({} steps) starting...", kWarmUpSteps);
        // Temporarily lower totalSteps so the warm-up is as fast as possible.
        // Direct access to params_ is safe here: warmUp() is only called from
        // initialize(), which runs before any worker threads are started.
        const SynthesisParams saved = params_;
        params_.totalSteps = kWarmUpSteps;
        synthesize("hello");
        params_ = saved;
        spdlog::debug("SupertonicTTS: warm-up inference done.");
    } catch (const std::exception& e) {
        spdlog::warn("SupertonicTTS: warm-up inference failed (non-fatal): {}", e.what());
    }
}

void SupertonicInferenceImpl::setParams(const SynthesisParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    params_ = params;
}

SupertonicInferenceImpl::SynthesisParams SupertonicInferenceImpl::getParams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return params_;
}

void SupertonicInferenceImpl::setLanguage(const std::string& lang) {
    std::lock_guard<std::mutex> lock(mutex_);
    lang_ = lang;
}

int SupertonicInferenceImpl::getSampleRate() const {
    return tts_ ? tts_->getSampleRate() : 0;
}
