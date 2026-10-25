// phonon.cpp — PhononTTS inference engine.
//
// Implements: GGML helpers, weight loading, multi-head attention,
// Flow-LM transformer layers, the flow network, text preprocessing,
// and the core synthesis loop (PhononTTS::Impl::run_synthesis).
//
// Mimi decoder lives in mimi.cpp.
// Convolution and math primitives live in ops.h (header-only).

#include "phonon.h"
#include "model_types.h"
#include "mimi.h"
#include "ops.h"
#include "normalize.h"
#include "chunk.h"

#include <ggml.h>
#include <ggml-cpu.h>
#include <gguf.h>

#include <sentencepiece_processor.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gradium::tts {

    // ---------------------------------------------------------------------------
    // GGML linear: y[seq, out] = x[seq, in] @ W^T + bias
    // W may be any dtype GGML supports (F32, BF16, Q8_0, …).
    // ---------------------------------------------------------------------------
    void linear_ggml(ggml_tensor* W,
        const float* x, int seq, int in, int out,
        const float* bias,
        float* y) {
        auto& threadContext = ThreadContext::current();

        // Wrap x as a no-alloc tensor view — ggml_mul_mat only reads it, so the
        // input never needs to be copied into the compute context.
        uint8_t hdr_buf[4096];
        ggml_context* hctx = ggml_init({ sizeof(hdr_buf), hdr_buf, /*no_alloc=*/true });
        ggml_tensor* xt = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, in, seq);
        xt->data = const_cast<float*>(x);

        size_t mem = (size_t)seq * out * sizeof(float) + 2ul * 1024 * 1024;
        if (threadContext.ggml_ctx_buf.size() < mem) threadContext.ggml_ctx_buf.resize(mem);
        struct ggml_init_params p = { mem, threadContext.ggml_ctx_buf.data(), false };
        ggml_context* ctx = ggml_init(p);

        ggml_tensor* res = ggml_mul_mat(ctx, W, xt);

        ggml_cgraph* gf = ggml_new_graph(ctx);
        ggml_build_forward_expand(gf, res);

        struct ggml_cplan cplan = ggml_graph_plan(gf, threadContext.num_threads, nullptr);
        if (cplan.work_size > threadContext.ggml_work_buf.size())
            threadContext.ggml_work_buf.resize(cplan.work_size);
        cplan.work_data = cplan.work_size > 0 ? threadContext.ggml_work_buf.data() : nullptr;
        ggml_graph_compute(gf, &cplan);

        const float* rp = (const float*)res->data;
        if (bias) {
            for (int s = 0; s < seq; s++)
                for (int o = 0; o < out; o++)
                    y[s * out + o] = rp[s * out + o] + bias[o];
        }
        else {
            memcpy(y, rp, (size_t)seq * out * sizeof(float));
        }

        ggml_free(ctx);
        ggml_free(hctx);
    }

    // ---------------------------------------------------------------------------
    // Dequantise any GGML tensor to a flat F32 vector.
    // ---------------------------------------------------------------------------
    void dequant_f32(const ggml_tensor* t, std::vector<float>& out) {
        int64_t n = ggml_nelements(t);
        out.resize(n);
        switch (t->type) {
        case GGML_TYPE_F32:
        memcpy(out.data(), t->data, n * sizeof(float));
        break;
        case GGML_TYPE_F16: {
            const ggml_fp16_t* src = (const ggml_fp16_t*)t->data;
            for (int64_t i = 0; i < n; i++) out[i] = ggml_fp16_to_fp32(src[i]);
            break;
        }
        case GGML_TYPE_BF16: {
            const uint16_t* src = (const uint16_t*)t->data;
            for (int64_t i = 0; i < n; i++) {
                uint32_t v = (uint32_t)src[i] << 16;
                memcpy(&out[i], &v, 4);
            }
            break;
        }
        case GGML_TYPE_Q8_0: {
            constexpr int QK = 32;
            const uint8_t* data = (const uint8_t*)t->data;
            int64_t nb = n / QK;
            for (int64_t b = 0; b < nb; b++) {
                const uint8_t* blk = data + b * (2 + QK);
                ggml_fp16_t d_h; memcpy(&d_h, blk, 2);
                float d = ggml_fp16_to_fp32(d_h);
                const int8_t* qs = (const int8_t*)(blk + 2);
                for (int q = 0; q < QK; q++) out[b * QK + q] = d * (float)qs[q];
            }
            break;
        }
        default:
        throw std::runtime_error(std::string("dequant_f32: unsupported type ")
            + ggml_type_name(t->type));
        }
    }

    // ---------------------------------------------------------------------------
    // Minimal safetensors reader — one voice tensor.
    //
    // Two layouts exist:
    //   "emb"          [T, d_model] or [1, T, d_model]: a precomputed voice embedding.
    //   "speaker_wavs" [C, T] or [1, C, T]: speaker-Mimi latents (the published
    //                  Phonon voices); transposed to [T, C] and projected to d_model
    //                  by the checkpoint's speaker_wavs.output_proj.
    // A file whose single tensor has another name is read like "emb".
    // ---------------------------------------------------------------------------
    namespace safetensors {

        struct VoiceTensor {
            std::string name;
            std::vector<int64_t> shape;
            std::vector<float> data;
        };

        static std::vector<int64_t> parse_int_array(const std::string& s, size_t pos) {
            std::vector<int64_t> out;
            size_t p = s.find('[', pos) + 1;
            while (p < s.size() && s[p] != ']') {
                if (std::isdigit((unsigned char)s[p])) {
                    int64_t v = 0;
                    while (p < s.size() && std::isdigit((unsigned char)s[p]))
                        v = v * 10 + (s[p++] - '0');
                    out.push_back(v);
                }
                else { p++; }
            }
            return out;
        }

        static VoiceTensor load_voice_tensor(const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            if (!f) throw std::runtime_error("Cannot open safetensors: " + path);
            uint64_t header_len = 0;
            f.read(reinterpret_cast<char*>(&header_len), 8);
            if (!f || header_len == 0 || header_len > (64u << 20))
                throw std::runtime_error("Not a safetensors file: " + path);
            std::string header(header_len, '\0');
            f.read(&header[0], header_len);

            VoiceTensor vt;
            size_t ep = std::string::npos;
            for (const char* candidate : { "emb", "speaker_wavs" }) {
                ep = header.find(std::string("\"") + candidate + "\"");
                if (ep != std::string::npos) { vt.name = candidate; break; }
            }
            if (ep == std::string::npos) {
                // First key that is not __metadata__.
                size_t p = 0;
                while ((p = header.find('"', p)) != std::string::npos) {
                    size_t e = header.find('"', p + 1);
                    std::string key = header.substr(p + 1, e - p - 1);
                    if (key != "__metadata__" && header.find("\"dtype\"", e) != std::string::npos) {
                        vt.name = key; ep = p; break;
                    }
                    p = header.find('}', e);
                    if (p == std::string::npos) break;
                }
            }
            if (ep == std::string::npos)
                throw std::runtime_error("No voice tensor in " + path);

            size_t dp = header.find("\"dtype\"", ep);
            size_t dq = header.find('"', header.find(':', dp)) + 1;
            std::string dtype = header.substr(dq, header.find('"', dq) - dq);
            vt.shape = parse_int_array(header, header.find("\"shape\"", ep));
            auto offsets = parse_int_array(header, header.find("\"data_offsets\"", ep));
            if (offsets.size() != 2) throw std::runtime_error("Bad data_offsets in " + path);

            int64_t n = 1; for (auto s : vt.shape) n *= s;
            const int64_t bytes = offsets[1] - offsets[0];
            f.seekg(static_cast<std::streampos>(8 + header_len + offsets[0]));
            vt.data.resize(n);
            if (dtype == "F32" && bytes == n * 4) {
                f.read(reinterpret_cast<char*>(vt.data.data()), n * 4);
            }
            else if ((dtype == "BF16" || dtype == "F16") && bytes == n * 2) {
                std::vector<uint16_t> raw(n);
                f.read(reinterpret_cast<char*>(raw.data()), n * 2);
                for (int64_t i = 0; i < n; i++) {
                    if (dtype == "BF16") {
                        uint32_t v = (uint32_t)raw[i] << 16;
                        memcpy(&vt.data[i], &v, 4);
                    }
                    else {
                        vt.data[i] = ggml_fp16_to_fp32(raw[i]);
                    }
                }
            }
            else {
                throw std::runtime_error("Unsupported voice dtype " + dtype + " in " + path);
            }
            if (!f) throw std::runtime_error("Truncated safetensors: " + path);
            return vt;
        }

    } // namespace safetensors

    // ---------------------------------------------------------------------------
    // PhononTTS::Impl — engine state + async stream registry
    // ---------------------------------------------------------------------------
    struct PhononTTS::PhononTTSImpl {
        Config       cfg;
        ModelWeights weights;
        ggml_context* ctx_weights = nullptr;
        gguf_context* gguf_ctx = nullptr;
        std::unique_ptr<sentencepiece::SentencePieceProcessor> tokenizer;

        // text::Tokenizer over the SentencePiece model, for the chunker.
        struct SpmTokenizer final : text::Tokenizer {
            const sentencepiece::SentencePieceProcessor* spm = nullptr;
            std::vector<int> encode(std::string_view text) const override {
                std::vector<int> ids;
                if (!spm->Encode(std::string(text), &ids).ok())
                    throw std::runtime_error("Tokenization failed");
                return ids;
            }
            std::string decode(const std::vector<int>& tokens) const override {
                std::string out;
                if (!spm->Decode(tokens, &out).ok())
                    throw std::runtime_error("Detokenization failed");
                return out;
            }
        } textTokenizer;
        int num_threads = 1;

        struct StreamState {
            std::thread       thread;
            std::atomic<bool> cancelled{ false };
            StreamState() = default;
            StreamState(const StreamState&) = delete;
            StreamState& operator=(const StreamState&) = delete;
        };
        std::atomic<StreamId>                                      _next_stream_id{ 1 };
        std::mutex                                                 _streams_mutex;
        std::unordered_map<StreamId, std::unique_ptr<StreamState>> _active_streams;

        // KV of the voice prefix (positions 0..voice_len), computed once at load
        // time — it is identical for every synthesis call.
        KVCache voice_kv;

        // Pool of per-call KV caches so back-to-back calls reuse the same
        // allocation instead of re-allocating (and faulting in) ~50 MB each time.
        std::mutex                            _kv_pool_mutex;
        std::vector<std::unique_ptr<KVCache>> _kv_pool;

        std::unique_ptr<KVCache> acquire_kv() {
            std::lock_guard<std::mutex> lock(_kv_pool_mutex);
            if (!_kv_pool.empty()) {
                auto kv = std::move(_kv_pool.back());
                _kv_pool.pop_back();
                return kv;
            }
            return std::make_unique<KVCache>();
        }
        void release_kv(std::unique_ptr<KVCache> kv) {
            std::lock_guard<std::mutex> lock(_kv_pool_mutex);
            _kv_pool.push_back(std::move(kv));
        }

        void load_weights(const Paths& paths);
        void load_voice(const std::string& path);
        void prefill_voice();
        std::vector<float> run_synthesis(StreamId id,
            const std::string& text,
            const SynthOpts& opts,
            SynthesisListener* listener,
            const std::atomic<bool>* cancel);
        ~PhononTTSImpl();
    };

    PhononTTS::PhononTTSImpl::~PhononTTSImpl() {
        // Set cancelled on every active stream, then join outside the lock.
        std::unordered_map<StreamId, std::unique_ptr<StreamState>> to_join;
        {
            std::lock_guard<std::mutex> lock(_streams_mutex);
            for (auto& [id, state] : _active_streams)
                state->cancelled.store(true, std::memory_order_relaxed);
            to_join = std::move(_active_streams);
        }
        for (auto& [id, state] : to_join)
            if (state && state->thread.joinable())
                state->thread.join();

        if (gguf_ctx)    gguf_free(gguf_ctx);
        if (ctx_weights) ggml_free(ctx_weights);
    }

    // ---------------------------------------------------------------------------
    // Config parser (hand-rolled, no external JSON dep)
    // ---------------------------------------------------------------------------
    static Config parse_config_impl(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot open config: " + path);
        std::string jsonContent((std::istreambuf_iterator<char>(f)), {});
        Config cfg;
        auto readInt = [&](const std::string& key, int defaultValue) -> int {
            auto pos = jsonContent.find('"' + key + '"'); if (pos == std::string::npos) return defaultValue;
            pos = jsonContent.find(':', pos);
            while (pos < jsonContent.size() && !std::isdigit((unsigned char)jsonContent[pos]) && jsonContent[pos] != '-') pos++;
            try { return std::stoi(jsonContent.substr(pos)); }
            catch (...) { return defaultValue; }
            };
        auto readFloat = [&](const std::string& key, float defaultValue) -> float {
            auto pos = jsonContent.find('"' + key + '"'); if (pos == std::string::npos) return defaultValue;
            pos = jsonContent.find(':', pos);
            while (pos < jsonContent.size() && !std::isdigit((unsigned char)jsonContent[pos]) && jsonContent[pos] != '-') pos++;
            try { return std::stof(jsonContent.substr(pos, 30)); }
            catch (...) { return defaultValue; }
            };
        cfg.flow_lm.d_model = readInt("d_model", 768);
        cfg.flow_lm.num_heads = readInt("num_heads", 12);
        cfg.flow_lm.num_layers = readInt("num_layers", 12);
        cfg.flow_lm.dim_feedforward = readInt("dim_feedforward", 3072);
        cfg.flow_lm.max_period = readFloat("max_period", 10000.f);
        cfg.flow_lm.n_bins = readInt("n_bins", 4000);
        cfg.flow_lm.flow_dim = readInt("flow_dim", 512);
        cfg.flow_lm.flow_depth = readInt("flow_depth", 6);
        cfg.flow_lm.ldim = readInt("ldim", 32);
        cfg.mimi.sample_rate = readInt("sample_rate", 24000);
        cfg.mimi.frame_rate = readFloat("frame_rate", 12.5f);
        cfg.mimi.dimension = readInt("dimension", 512);
        cfg.mimi.quantizer_dim = readInt("quantizer_dimension", 32);
        cfg.mimi.n_filters = readInt("n_filters", 64);
        cfg.mimi.transformer_d_model = readInt("transformer_d_model", 512);
        cfg.mimi.transformer_num_heads = readInt("transformer_num_heads", 8);
        cfg.mimi.transformer_num_layers = readInt("transformer_num_layers", 2);
        cfg.mimi.transformer_layer_scale = readFloat("transformer_layer_scale", 0.01f);
        cfg.mimi.transformer_context = readInt("transformer_context", 250);
        cfg.mimi.transformer_max_period = readFloat("transformer_max_period", 10000.f);
        cfg.temp = readFloat("temp", 0.3f);
        cfg.lsd_decode_steps = readInt("lsd_decode_steps", 1);
        cfg.eos_threshold = readFloat("eos_threshold", -4.f);

        // The first "gelu" key is flow_lm's. Absent: erf for Phonon exports (they carry a
        // model_id), tanh for Pocket TTS checkpoints.
        auto gp = jsonContent.find("\"gelu\"");
        if (gp != std::string::npos) {
            auto q = jsonContent.find('"', jsonContent.find(':', gp)) + 1;
            cfg.gelu_erf = jsonContent.compare(q, 3, "erf") == 0;
        }
        else {
            cfg.gelu_erf = jsonContent.find("\"model_id\"") != std::string::npos;
        }
        // Flat keys from the HF export; the nested conditioner block reuses generic
        // names ("dim", "max_period") that collide with other sections.
        cfg.duration_delta_scale = readFloat("duration_delta_scale", 1000.f);
        cfg.duration_delta_max_period = readFloat("duration_delta_max_period", 10000.f);
        return cfg;
    }

    // ---------------------------------------------------------------------------
    // Weight loading
    // ---------------------------------------------------------------------------
    void PhononTTS::PhononTTSImpl::load_weights(const Paths& paths) {
        const auto& flowConfig = cfg.flow_lm;
        const auto& mimiConfig = cfg.mimi;
        auto& modelWeights = weights;
        ggml_context* ctx = ctx_weights;

        auto getTensor = [&](const std::string& name) -> ggml_tensor* {
            auto* tensor = ggml_get_tensor(ctx, name.c_str());
            if (!tensor) throw std::runtime_error("Missing tensor: " + name);
            return tensor;
            };
        auto getDequantized = [&](const std::string& name) -> std::vector<float> {
            std::vector<float> out;
            dequant_f32(getTensor(name), out);
            return out;
            };

        // Flow-LM transformer layers
        modelWeights.flow_layers.resize(flowConfig.num_layers);
        for (int l = 0; l < flowConfig.num_layers; l++) {
            std::string prefix = "flow_lm.transformer.layers." + std::to_string(l) + ".";
            auto& layerWeights = modelWeights.flow_layers[l];
            layerWeights.in_proj = getTensor(prefix + "self_attn.in_proj.weight");
            layerWeights.out_proj = getTensor(prefix + "self_attn.out_proj.weight");
            layerWeights.linear1 = getTensor(prefix + "linear1.weight");
            layerWeights.linear2 = getTensor(prefix + "linear2.weight");
            layerWeights.norm1_w = getDequantized(prefix + "norm1.weight"); layerWeights.norm1_b = getDequantized(prefix + "norm1.bias");
            layerWeights.norm2_w = getDequantized(prefix + "norm2.weight"); layerWeights.norm2_b = getDequantized(prefix + "norm2.bias");
        }
        modelWeights.out_norm_w = getDequantized("flow_lm.out_norm.weight");
        modelWeights.out_norm_b = getDequantized("flow_lm.out_norm.bias");
        modelWeights.out_eos_w = getDequantized("flow_lm.out_eos.weight");
        modelWeights.out_eos_b = getDequantized("flow_lm.out_eos.bias");
        modelWeights.bos_emb = getDequantized("flow_lm.bos_emb");
        modelWeights.emb_mean = getDequantized("flow_lm.emb_mean");
        modelWeights.emb_std = getDequantized("flow_lm.emb_std");
        modelWeights.input_linear = getTensor("flow_lm.input_linear.weight");
        modelWeights.txt_embed = getDequantized("flow_lm.condition_provider.conditioners.transcript_in_segment.embed.weight");
        modelWeights.spk_lut = getDequantized("flow_lm.condition_provider.conditioners.num_speakers.embed.weight");
        modelWeights.spk_out_w = getTensor("flow_lm.condition_provider.conditioners.num_speakers.output_proj.weight");
        {
            auto* speakerLutTensor = ggml_get_tensor(ctx, "flow_lm.condition_provider.conditioners.num_speakers.embed.weight");
            modelWeights.spk_lut_d = speakerLutTensor ? (int)speakerLutTensor->ne[0] : 16;
        }
        // Optional on older checkpoints.
        modelWeights.spk_wavs_proj = ggml_get_tensor(ctx,
            "flow_lm.condition_provider.conditioners.speaker_wavs.output_proj.weight");
        modelWeights.dur_out_w = ggml_get_tensor(ctx,
            "flow_lm.condition_provider.conditioners.duration_delta.output_proj.weight");
        modelWeights.dur_dim = modelWeights.dur_out_w ? (int)modelWeights.dur_out_w->ne[0] : 0;

        // Flow network
        modelWeights.fp_in_w = getTensor("flow_lm.flow_net.input_proj.weight");
        modelWeights.fp_in_b = getDequantized("flow_lm.flow_net.input_proj.bias");
        modelWeights.fp_ce_w = getTensor("flow_lm.flow_net.cond_embed.weight");
        modelWeights.fp_ce_b = getDequantized("flow_lm.flow_net.cond_embed.bias");

        modelWeights.time_embeds.resize(2);
        for (int i = 0; i < 2; i++) {
            std::string prefix = "flow_lm.flow_net.time_embed." + std::to_string(i) + ".";
            auto& timeEmbed = modelWeights.time_embeds[i];
            timeEmbed.freqs = getDequantized(prefix + "freqs");
            timeEmbed.n_freqs = (int)timeEmbed.freqs.size();
            timeEmbed.mlp0_w = getTensor(prefix + "mlp.0.weight"); timeEmbed.mlp0_b = getDequantized(prefix + "mlp.0.bias");
            timeEmbed.mlp2_w = getTensor(prefix + "mlp.2.weight"); timeEmbed.mlp2_b = getDequantized(prefix + "mlp.2.bias");
            timeEmbed.alpha = getDequantized(prefix + "mlp.3.alpha");
        }

        modelWeights.res_blocks.resize(flowConfig.flow_depth);
        for (int i = 0; i < flowConfig.flow_depth; i++) {
            std::string prefix = "flow_lm.flow_net.res_blocks." + std::to_string(i) + ".";
            auto& resBlock = modelWeights.res_blocks[i];
            resBlock.mlp0_w = getTensor(prefix + "mlp.0.weight"); resBlock.mlp0_b = getDequantized(prefix + "mlp.0.bias");
            resBlock.mlp2_w = getTensor(prefix + "mlp.2.weight"); resBlock.mlp2_b = getDequantized(prefix + "mlp.2.bias");
            resBlock.ada_w = getTensor(prefix + "adaLN_modulation.1.weight");
            resBlock.ada_b = getDequantized(prefix + "adaLN_modulation.1.bias");
            resBlock.in_ln_w = getDequantized(prefix + "in_ln.weight"); resBlock.in_ln_b = getDequantized(prefix + "in_ln.bias");
        }
        modelWeights.fp_fl_ada_w = getTensor("flow_lm.flow_net.final_layer.adaLN_modulation.1.weight");
        modelWeights.fp_fl_ada_b = getDequantized("flow_lm.flow_net.final_layer.adaLN_modulation.1.bias");
        modelWeights.fp_fl_lin_w = getTensor("flow_lm.flow_net.final_layer.linear.weight");
        modelWeights.fp_fl_lin_b = getDequantized("flow_lm.flow_net.final_layer.linear.bias");

        // Mimi codec — quantizer output projection is Conv1d(k=1), stored as
        // 3D [out,in,1] in GGUF.  Dequantise to 2D for a plain matmul.
        modelWeights.mimi_qout_w = getDequantized("mimi.quantizer.output_proj.weight");
        modelWeights.mimi_up_w = getDequantized("mimi.upsample.convtr.convtr.weight");

        modelWeights.mimi_tr_layers.resize(mimiConfig.transformer_num_layers);
        for (int l = 0; l < mimiConfig.transformer_num_layers; l++) {
            std::string prefix = "mimi.decoder_transformer.transformer.layers." + std::to_string(l) + ".";
            auto& layerWeights = modelWeights.mimi_tr_layers[l];
            layerWeights.in_proj = getTensor(prefix + "self_attn.in_proj.weight");
            layerWeights.out_proj = getTensor(prefix + "self_attn.out_proj.weight");
            layerWeights.linear1 = getTensor(prefix + "linear1.weight");
            layerWeights.linear2 = getTensor(prefix + "linear2.weight");
            layerWeights.norm1_w = getDequantized(prefix + "norm1.weight"); layerWeights.norm1_b = getDequantized(prefix + "norm1.bias");
            layerWeights.norm2_w = getDequantized(prefix + "norm2.weight"); layerWeights.norm2_b = getDequantized(prefix + "norm2.bias");
            layerWeights.ls1 = getDequantized(prefix + "layer_scale_1.scale");
            layerWeights.ls2 = getDequantized(prefix + "layer_scale_2.scale");
        }

        auto loadCnnLayer = [&](std::vector<float>& weightVec, std::vector<float>& biasVec,
            const std::string& weightName, const std::string& biasName) {
                weightVec = getDequantized(weightName); biasVec = getDequantized(biasName);
            };
        loadCnnLayer(modelWeights.d_c0_w, modelWeights.d_c0_b, "mimi.decoder.model.0.conv.weight", "mimi.decoder.model.0.conv.bias");
        loadCnnLayer(modelWeights.d_t2_w, modelWeights.d_t2_b, "mimi.decoder.model.2.convtr.weight", "mimi.decoder.model.2.convtr.bias");
        loadCnnLayer(modelWeights.d_r3a_w, modelWeights.d_r3a_b, "mimi.decoder.model.3.block.1.conv.weight", "mimi.decoder.model.3.block.1.conv.bias");
        loadCnnLayer(modelWeights.d_r3b_w, modelWeights.d_r3b_b, "mimi.decoder.model.3.block.3.conv.weight", "mimi.decoder.model.3.block.3.conv.bias");
        loadCnnLayer(modelWeights.d_t5_w, modelWeights.d_t5_b, "mimi.decoder.model.5.convtr.weight", "mimi.decoder.model.5.convtr.bias");
        loadCnnLayer(modelWeights.d_r6a_w, modelWeights.d_r6a_b, "mimi.decoder.model.6.block.1.conv.weight", "mimi.decoder.model.6.block.1.conv.bias");
        loadCnnLayer(modelWeights.d_r6b_w, modelWeights.d_r6b_b, "mimi.decoder.model.6.block.3.conv.weight", "mimi.decoder.model.6.block.3.conv.bias");
        loadCnnLayer(modelWeights.d_t8_w, modelWeights.d_t8_b, "mimi.decoder.model.8.convtr.weight", "mimi.decoder.model.8.convtr.bias");
        loadCnnLayer(modelWeights.d_r9a_w, modelWeights.d_r9a_b, "mimi.decoder.model.9.block.1.conv.weight", "mimi.decoder.model.9.block.1.conv.bias");
        loadCnnLayer(modelWeights.d_r9b_w, modelWeights.d_r9b_b, "mimi.decoder.model.9.block.3.conv.weight", "mimi.decoder.model.9.block.3.conv.bias");
        loadCnnLayer(modelWeights.d_c11_w, modelWeights.d_c11_b, "mimi.decoder.model.11.conv.weight", "mimi.decoder.model.11.conv.bias");

        // Rearrange CNN weights from PyTorch layout to im2col/axpy layout.
        const int mimiDimension = mimiConfig.dimension;
        conv::rearrange_conv1d_w(modelWeights.d_c0_w, mimiDimension, mimiDimension, 7);
        conv::rearrange_conv1d_w(modelWeights.d_r3a_w, 128, 256, 3);
        conv::rearrange_conv1d_w(modelWeights.d_r3b_w, 256, 128, 1);
        conv::rearrange_conv1d_w(modelWeights.d_r6a_w, 64, 128, 3);
        conv::rearrange_conv1d_w(modelWeights.d_r6b_w, 128, 64, 1);
        conv::rearrange_conv1d_w(modelWeights.d_r9a_w, 32, 64, 3);
        conv::rearrange_conv1d_w(modelWeights.d_r9b_w, 64, 32, 1);
        conv::rearrange_conv1d_w(modelWeights.d_c11_w, 1, 64, 3);
        conv::rearrange_convtr1d_w(modelWeights.d_t2_w, mimiDimension, 256, 12);
        conv::rearrange_convtr1d_w(modelWeights.d_t5_w, 256, 128, 10);
        conv::rearrange_convtr1d_w(modelWeights.d_t8_w, 128, 64, 8);
        conv::rearrange_dw_w(modelWeights.mimi_up_w, mimiDimension, 32);

        load_voice(paths.voice);

        std::cerr << "[phonon] Loading tokenizer: " << paths.tokenizer << "\n";
        tokenizer = std::make_unique<sentencepiece::SentencePieceProcessor>();
        if (!tokenizer->Load(paths.tokenizer).ok())
            throw std::runtime_error("SentencePiece load failed: " + paths.tokenizer);
        textTokenizer.spm = tokenizer.get();
    }

    // ---------------------------------------------------------------------------
    // Multi-head causal attention with RoPE and KV-cache.
    // window=0 → full causal; window>0 → sliding causal window.
    // ---------------------------------------------------------------------------
    void mha_fwd(const float* x, int seq, int hiddenDim, int num_heads,
        ggml_tensor* W_in, ggml_tensor* W_out,
        float* keyCache, float* valueCache,
        int past, int max_ctx, float max_period,
        float* out, int window, int pos_base) {
        const int headDim = hiddenDim / num_heads;
        const int qkvDim = 3 * hiddenDim;
        const int halfDim = headDim / 2;

        // Per-thread scratch, reused across calls (grows only).
        thread_local std::vector<float> qkv, headOutput, scores, Q, K, V,
            ropeCos, ropeSin;

        qkv.resize((size_t)seq * qkvDim);
        linear_ggml(W_in, x, seq, hiddenDim, qkvDim, nullptr, qkv.data());

        // RoPE rotation tables for positions past..past+seq-1 — identical for
        // every head, so compute cos/sin once instead of per head.
        const float* invFreq = math::rope_inv_freqs(headDim, max_period);
        ropeCos.resize((size_t)seq * halfDim);
        ropeSin.resize((size_t)seq * halfDim);
        for (int s = 0; s < seq; s++) {
            for (int i = 0; i < halfDim; i++) {
                const float a = (float)(pos_base + past + s) * invFreq[i];
                ropeCos[(size_t)s * halfDim + i] = std::cos(a);
                ropeSin[(size_t)s * halfDim + i] = std::sin(a);
            }
        }

        headOutput.resize((size_t)seq * hiddenDim);
        const float scale = 1.f / std::sqrt((float)headDim);

        const int maxKvLen = (window > 0) ? window : (past + seq);
        scores.resize(maxKvLen);

        Q.resize((size_t)seq * headDim);
        K.resize((size_t)seq * headDim);
        V.resize((size_t)seq * headDim);

        for (int h = 0; h < num_heads; h++) {
            for (int s = 0; s < seq; s++) {
                const float* row = qkv.data() + s * qkvDim;
                memcpy(Q.data() + s * headDim, row + h * headDim, headDim * sizeof(float));
                memcpy(K.data() + s * headDim, row + hiddenDim + h * headDim, headDim * sizeof(float));
                memcpy(V.data() + s * headDim, row + 2 * hiddenDim + h * headDim, headDim * sizeof(float));
                const float* c = ropeCos.data() + (size_t)s * halfDim;
                const float* sn = ropeSin.data() + (size_t)s * halfDim;
                math::apply_rope_cached(Q.data() + s * headDim, halfDim, c, sn);
                math::apply_rope_cached(K.data() + s * headDim, halfDim, c, sn);
            }

            float* keyCacheHead = keyCache + h * max_ctx * headDim;
            float* valueCacheHead = valueCache + h * max_ctx * headDim;
            for (int s = 0; s < seq; s++) {
                memcpy(keyCacheHead + (past + s) * headDim, K.data() + s * headDim, headDim * sizeof(float));
                memcpy(valueCacheHead + (past + s) * headDim, V.data() + s * headDim, headDim * sizeof(float));
            }

            for (int qi = 0; qi < seq; qi++) {
                const float* query = Q.data() + qi * headDim;
                int kvEnd = past + qi + 1;
                int kvStart = (window > 0) ? std::max(0, kvEnd - window) : 0;
                int kvLen = kvEnd - kvStart;
                for (int ki = 0; ki < kvLen; ki++) {
                    const float* key = keyCacheHead + (kvStart + ki) * headDim;
                    float dot = 0.f;
                    for (int j = 0; j < headDim; j++) dot += query[j] * key[j];
                    scores[ki] = dot * scale;
                }
                math::softmax_inplace(scores.data(), kvLen);
                float* headSlice = headOutput.data() + qi * hiddenDim + h * headDim;
                memset(headSlice, 0, headDim * sizeof(float));
                for (int ki = 0; ki < kvLen; ki++) {
                    const float* value = valueCacheHead + (kvStart + ki) * headDim;
                    for (int j = 0; j < headDim; j++) headSlice[j] += scores[ki] * value[j];
                }
            }
        }

        linear_ggml(W_out, headOutput.data(), seq, hiddenDim, hiddenDim, nullptr, out);
    }

    // ---------------------------------------------------------------------------
    // Flow-LM transformer layer (in-place, pre-norm, GELU FFN)
    // ---------------------------------------------------------------------------
    static void flow_lm_layer(float* x, int seq, const Config& cfg,
        const LayerW& layerWeights, float* keyCache, float* valueCache,
        int past, int max_ctx) {
        const int   modelDim = cfg.flow_lm.d_model;
        const int   feedforwardDim = cfg.flow_lm.dim_feedforward;
        const int   numHeads = cfg.flow_lm.num_heads;
        const float maxPeriod = cfg.flow_lm.max_period;

        // Per-thread scratch, reused across calls (grows only).
        thread_local std::vector<float> preLnBuffer, attentionOutput, ffnHidden, ffnOutput;
        preLnBuffer.resize((size_t)seq * modelDim);
        attentionOutput.resize((size_t)seq * modelDim);
        ffnHidden.resize((size_t)seq * feedforwardDim);
        ffnOutput.resize((size_t)seq * modelDim);

        // Pre-norm → attention → residual
        for (int s = 0; s < seq; s++)
            math::layer_norm(x + s * modelDim, modelDim,
                layerWeights.norm1_w.data(), layerWeights.norm1_b.data(),
                1e-5f, preLnBuffer.data() + s * modelDim);
        mha_fwd(preLnBuffer.data(), seq, modelDim, numHeads,
            layerWeights.in_proj, layerWeights.out_proj,
            keyCache, valueCache, past, max_ctx, maxPeriod, attentionOutput.data());
        for (int i = 0; i < seq * modelDim; i++) x[i] += attentionOutput[i];

        // Pre-norm → GELU FFN → residual
        for (int s = 0; s < seq; s++)
            math::layer_norm(x + s * modelDim, modelDim,
                layerWeights.norm2_w.data(), layerWeights.norm2_b.data(),
                1e-5f, preLnBuffer.data() + s * modelDim);
        linear_ggml(layerWeights.linear1, preLnBuffer.data(), seq, modelDim, feedforwardDim, nullptr, ffnHidden.data());
        math::gelu_inplace(ffnHidden.data(), ffnHidden.size(), cfg.gelu_erf);
        linear_ggml(layerWeights.linear2, ffnHidden.data(), seq, feedforwardDim, modelDim, nullptr, ffnOutput.data());
        for (int i = 0; i < seq * modelDim; i++) x[i] += ffnOutput[i];
    }

    // ---------------------------------------------------------------------------
    // Timestep embedder: scalar → sinusoidal → MLP → RMSNorm
    // ---------------------------------------------------------------------------
    static void embed_timestep(float timestepValue, const TimeEmbedW& timeEmbedWeights,
        int flowDim, float* out) {
        const int numFreqs = timeEmbedWeights.n_freqs;
        std::vector<float> sinusoidalEmbedding(2 * numFreqs);
        math::sinusoidal_embed(timestepValue, timeEmbedWeights.freqs.data(), numFreqs, sinusoidalEmbedding.data());

        std::vector<float> hidden(flowDim);
        linear_ggml(timeEmbedWeights.mlp0_w, sinusoidalEmbedding.data(), 1, 2 * numFreqs, flowDim,
            timeEmbedWeights.mlp0_b.data(), hidden.data());
        for (auto& v : hidden) v = math::silu(v);

        std::vector<float> hiddenNormed(flowDim);
        linear_ggml(timeEmbedWeights.mlp2_w, hidden.data(), 1, flowDim, flowDim,
            timeEmbedWeights.mlp2_b.data(), hiddenNormed.data());
        math::rms_norm(hiddenNormed.data(), flowDim, timeEmbedWeights.alpha.data(), 1e-5f, out);
    }

    // ---------------------------------------------------------------------------
    // Flow network — predicts velocity for the Euler integration step
    // ---------------------------------------------------------------------------
    static void flow_net_forward(const float* noiseLatent, const float* conditioning,
        const Config& cfg, const ModelWeights& modelWeights, float* velocityOut) {
        const int latentDim = cfg.flow_lm.ldim;
        const int flowDim = cfg.flow_lm.flow_dim;
        const int conditioningDim = cfg.flow_lm.d_model;

        // Per-thread scratch, reused across calls (grows only).
        thread_local std::vector<float> hidden, conditioningActivation,
            adaLNModulation, hiddenNormed, mlpHidden, mlpOutput;
        hidden.resize(flowDim);
        conditioningActivation.resize(flowDim);
        adaLNModulation.resize(3 * flowDim);
        hiddenNormed.resize(flowDim);
        mlpHidden.resize(flowDim);
        mlpOutput.resize(flowDim);

        // Project latent and conditioning into flow_dim
        linear_ggml(modelWeights.fp_in_w, noiseLatent, 1, latentDim, flowDim,
            modelWeights.fp_in_b.data(), hidden.data());

        // Conditioning projection + precomputed average of the t=0 / t=1
        // timestep embeddings (single Euler step), then the SiLU used by every
        // adaLN modulation — all invariant across the residual blocks.
        {
            thread_local std::vector<float> conditioningProjection;
            conditioningProjection.resize(flowDim);
            linear_ggml(modelWeights.fp_ce_w, conditioning, 1, conditioningDim, flowDim,
                modelWeights.fp_ce_b.data(), conditioningProjection.data());
            for (int i = 0; i < flowDim; i++)
                conditioningActivation[i] = math::silu(
                    conditioningProjection[i] + modelWeights.time_embed_avg[i]);
        }

        // Residual flow blocks with adaptive LayerNorm
        for (int b = 0; b < cfg.flow_lm.flow_depth; b++) {
            const auto& resBlock = modelWeights.res_blocks[b];

            linear_ggml(resBlock.ada_w, conditioningActivation.data(), 1, flowDim, 3 * flowDim,
                resBlock.ada_b.data(), adaLNModulation.data());
            const float* shift = adaLNModulation.data();
            const float* scale = adaLNModulation.data() + flowDim;
            const float* gate = adaLNModulation.data() + 2 * flowDim;

            math::layer_norm(hidden.data(), flowDim,
                resBlock.in_ln_w.data(), resBlock.in_ln_b.data(), 1e-6f, hiddenNormed.data());
            for (int i = 0; i < flowDim; i++) hiddenNormed[i] = hiddenNormed[i] * (1.f + scale[i]) + shift[i];

            linear_ggml(resBlock.mlp0_w, hiddenNormed.data(), 1, flowDim, flowDim,
                resBlock.mlp0_b.data(), mlpHidden.data());
            for (int i = 0; i < flowDim; i++) mlpHidden[i] = math::silu(mlpHidden[i]);

            linear_ggml(resBlock.mlp2_w, mlpHidden.data(), 1, flowDim, flowDim,
                resBlock.mlp2_b.data(), mlpOutput.data());
            for (int i = 0; i < flowDim; i++) hidden[i] += gate[i] * mlpOutput[i];
        }

        // Final layer: adaLN modulation then linear projection to latent dim
        linear_ggml(modelWeights.fp_fl_ada_w, conditioningActivation.data(), 1, flowDim, 2 * flowDim,
            modelWeights.fp_fl_ada_b.data(), adaLNModulation.data());
        const float* finalShift = adaLNModulation.data();
        const float* finalScale = adaLNModulation.data() + flowDim;

        math::layer_norm(hidden.data(), flowDim, nullptr, nullptr, 1e-6f, hiddenNormed.data());
        for (int i = 0; i < flowDim; i++) hiddenNormed[i] = hiddenNormed[i] * (1.f + finalScale[i]) + finalShift[i];
        linear_ggml(modelWeights.fp_fl_lin_w, hiddenNormed.data(), 1, flowDim, latentDim,
            modelWeights.fp_fl_lin_b.data(), velocityOut);
    }

    // ---------------------------------------------------------------------------
    // Core synthesis — called by both blocking and async paths
    // ---------------------------------------------------------------------------
    std::vector<float> PhononTTS::PhononTTSImpl::run_synthesis(
        StreamId id,
        const std::string& text,
        const SynthOpts& opts,
        SynthesisListener* listener,
        const std::atomic<bool>* cancel)
    {
        auto t_start = std::chrono::steady_clock::now();

        if (listener) listener->onStart(id, text);

        const Config& config = cfg;
        const auto& flowConfig = config.flow_lm;
        const ModelWeights& modelWeights = weights;
        const int modelDim = flowConfig.d_model;
        const int latentDim = flowConfig.ldim;

        // Tokenise. Text arrives already normalized (text::normalizeText, by the caller);
        // this is the reference's last step, ptts::tts_model::prepare_text_prompt.
        const text::Prompt prompt = text::prepareTextPrompt(text);
        const std::string& prep = prompt.text;
        std::vector<int> tokens;
        if (!tokenizer->Encode(prep, &tokens).ok())
            throw std::runtime_error("Tokenization failed");

        if (tokens.empty()) {
            if (listener) {
                SynthesisStats stats;
                stats.synthesis_ms = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - t_start).count();
                listener->onFinish(id, {}, stats);
            }
            return {};
        }

        const int numTextTokens = (int)tokens.size();
        std::cerr << "[phonon] stream#" << id << ": " << numTextTokens
            << " tokens (\"" << prep << "\")\n";

        // Speaker conditioning
        std::vector<float> speakerConditioning(modelDim, 0.f);
        {
            const float* lutRow = modelWeights.spk_lut.data() + opts.num_speakers * modelWeights.spk_lut_d;
            linear_ggml(modelWeights.spk_out_w, lutRow, 1, modelWeights.spk_lut_d, modelDim,
                nullptr, speakerConditioning.data());
        }
        // duration_delta is summed in alongside num_speakers. Its embedding is not zero at
        // delta = 0 (the cosine half is 1), so it must be added even with the default.
        if (modelWeights.dur_out_w) {
            const int dim = modelWeights.dur_dim;
            const int half = dim / 2;
            const float position = config.duration_delta_scale * opts.duration_delta;
            std::vector<float> emb(dim), proj(modelDim);
            for (int i = 0; i < half; i++) {
                const float phase = position /
                    std::pow(config.duration_delta_max_period, (float)i / (float)(half - 1));
                emb[i] = std::cos(phase);
                emb[i + half] = std::sin(phase);
            }
            linear_ggml(modelWeights.dur_out_w, emb.data(), 1, dim, modelDim, nullptr, proj.data());
            for (int i = 0; i < modelDim; i++) speakerConditioning[i] += proj[i];
        }

        // Prefix: cached voice KV followed by text token embeddings
        const int numVoiceTokens = modelWeights.voice_len;
        const int prefixLength = numVoiceTokens + numTextTokens;
        std::vector<float> textEmbeddings(numTextTokens * modelDim);
        for (int i = 0; i < numTextTokens; i++)
            memcpy(textEmbeddings.data() + i * modelDim,
                modelWeights.txt_embed.data() + tokens[i] * modelDim, modelDim * sizeof(float));

        static constexpr int MAX_CTX = 4096;
        int maxFrames = opts.max_frames > 0 ? opts.max_frames
            : (int)text::frameBudget(tokens.size(), config.mimi.frame_rate);
        maxFrames = std::min(maxFrames, MAX_CTX - prefixLength - 10);

        // Size the KV cache to this call instead of MAX_CTX, and reuse a pooled
        // allocation across calls.
        auto kvHolder = acquire_kv();
        KVCache& kvCache = *kvHolder;
        kvCache.ensure(flowConfig.num_layers, flowConfig.num_heads,
            prefixLength + maxFrames + 10, modelDim / flowConfig.num_heads);
        const int maxCtx = kvCache.ctx_len;

        // Initialise per-thread count (0 means this thread has not been configured yet).
        auto& threadContext = ThreadContext::current();
        if (threadContext.num_threads == 0) threadContext.num_threads = num_threads;
        if (opts.num_threads > 0) threadContext.num_threads = opts.num_threads;

        // The voice-prefix KV was computed once at load time; copy it in
        // (per head — the two caches have different ctx strides) and prefill
        // only the text tokens on top of it.
        {
            const int headDim = kvCache.hd;
            const int numHeads = kvCache.nh;
            for (int l = 0; l < flowConfig.num_layers; l++) {
                for (int h = 0; h < numHeads; h++) {
                    const size_t srcOff = (size_t)h * voice_kv.ctx_len * headDim;
                    const size_t dstOff = (size_t)h * maxCtx * headDim;
                    memcpy(kvCache.kl(l) + dstOff, voice_kv.kl(l) + srcOff,
                        (size_t)numVoiceTokens * headDim * sizeof(float));
                    memcpy(kvCache.vl(l) + dstOff, voice_kv.vl(l) + srcOff,
                        (size_t)numVoiceTokens * headDim * sizeof(float));
                }
            }
        }

        // Full concurrency for prefill (large sequence); 1 thread for the
        // per-frame decode loop.  Each decode step is a seq=1 forward pass:
        // every matmul produces only 1 output row, so GGML can dispatch work
        // to exactly 1 thread regardless of pool size.  Extra threads just spin
        // and burn CPU, causing thermal throttling that slows the one thread
        // actually doing work.  KleidiAI (enabled for Android in CMakeLists)
        // is the correct lever for faster single-threaded Q8_0 GEMV.
        const int threadsForPrefill = threadContext.num_threads;
        for (int l = 0; l < flowConfig.num_layers; l++)
            flow_lm_layer(textEmbeddings.data(), numTextTokens, config, modelWeights.flow_layers[l],
                kvCache.kl(l), kvCache.vl(l), numVoiceTokens, maxCtx);
        threadContext.num_threads = 1;
        int pastLength = prefixLength;

        // EOS grace period: allow a few frames after the model signals end-of-speech.
        const int framesAfterEos = prompt.framesAfterEos;

        std::mt19937 rng(opts.seed);
        std::normal_distribution<float> noiseDist(0.f, std::sqrt(opts.temperature));

        std::vector<std::vector<float>> collectedFrames;
        int eosCountdown = -1;

        // Next-token embedding, written in place into currentToken.
        std::vector<float> currentToken(modelDim);
        auto makeNextToken = [&](const float* latent) {
            linear_ggml(modelWeights.input_linear, latent, 1, latentDim, modelDim,
                nullptr, currentToken.data());
            for (int i = 0; i < modelDim; i++) currentToken[i] += speakerConditioning[i];
            };

        auto resample = [&](std::vector<float>& pcm) {
            if (opts.speed == 1.0f || opts.speed <= 0.f || pcm.empty()) return;
            const int inputLen = (int)pcm.size();
            const int outputLen = (int)((float)inputLen / opts.speed);
            std::vector<float> resampled(outputLen);
            for (int i = 0; i < outputLen; i++) {
                float fracPos = i * opts.speed;
                int   i0 = (int)fracPos;
                float frac = fracPos - (float)i0;
                int   i1 = std::min(i0 + 1, inputLen - 1);
                resampled[i] = pcm[i0] * (1.f - frac) + pcm[i1] * frac;
            }
            pcm = std::move(resampled);
            };

        std::vector<float> outputPcm;
        int totalStreamingSamples = 0;
        const bool streamingMode = (opts.stream_chunk_frames > 0 && listener != nullptr);

        // Streaming state: chunks are decoded and delivered *inside* the
        // generation loop, so the first audio arrives after chunkSize frames
        // instead of after the whole utterance.  The Mimi decoder is stateful —
        // every stage carries its exact left-context across chunks — so chunked
        // output is bit-identical to decoding the whole utterance at once, with
        // no overlap re-decoding.
        const int chunkSize = opts.stream_chunk_frames > 0 ? opts.stream_chunk_frames : 25;
        std::vector<std::vector<float>> pendingFrames;
        MimiStreamState mimiState;
        mimiState.max_chunk_frames = chunkSize;
        if (streamingMode) pendingFrames.reserve(chunkSize + 1);

        // Ramp-up schedule: the first chunk is small so audio starts flowing
        // early in the render, then the size doubles up to stream_chunk_frames.
        // The stateful decoder makes any chunking bit-exact, so this changes
        // only delivery timing, never the samples.
        constexpr int kFirstChunkFrames = 6; // 480 ms of audio
        int framesUntilFlush = std::min(chunkSize, kFirstChunkFrames);

        auto flushChunk = [&]() {
            if (pendingFrames.empty()) return;

            // Mimi decode is a large-sequence CNN/transformer — run it with the
            // full thread count, then drop back to the caller's setting (1
            // during the decode loop).
            const int callerThreads = threadContext.num_threads;
            threadContext.num_threads = threadsForPrefill;
            auto chunkPcm = mimi_decode_stream(pendingFrames, config, modelWeights, mimiState);
            threadContext.num_threads = callerThreads;
            pendingFrames.clear();

            resample(chunkPcm);
            totalStreamingSamples += (int)chunkPcm.size();
            listener->onAudioChunk(id, std::move(chunkPcm));
            };

        makeNextToken(modelWeights.bos_emb.data());

        // Per-frame buffers, hoisted out of the loop.
        std::vector<float> tokenHidden(modelDim), hiddenNormed(modelDim),
            noiseLatent(latentDim), velocity(latentDim);

        for (int step = 0; step < maxFrames; step++) {
            if (cancel && cancel->load(std::memory_order_relaxed)) break;

            // Transformer forward pass for this token
            tokenHidden = currentToken;
            for (int l = 0; l < flowConfig.num_layers; l++)
                flow_lm_layer(tokenHidden.data(), 1, config, modelWeights.flow_layers[l],
                    kvCache.kl(l), kvCache.vl(l), pastLength, maxCtx);
            pastLength++;

            math::layer_norm(tokenHidden.data(), modelDim,
                modelWeights.out_norm_w.data(), modelWeights.out_norm_b.data(),
                1e-5f, hiddenNormed.data());

            // EOS check — a 1-row dot product; no need for a GGML graph.
            float eosLogit = modelWeights.out_eos_b[0];
            for (int i = 0; i < modelDim; i++)
                eosLogit += hiddenNormed[i] * modelWeights.out_eos_w[i];
            const bool isEos = eosLogit > config.eos_threshold;

            // Sample noise → predict velocity → Euler step → new latent
            for (auto& v : noiseLatent) v = noiseDist(rng);
            flow_net_forward(noiseLatent.data(), hiddenNormed.data(), config, modelWeights, velocity.data());
            std::vector<float> generatedLatent(latentDim);
            for (int i = 0; i < latentDim; i++) generatedLatent[i] = noiseLatent[i] + velocity[i];

            makeNextToken(generatedLatent.data());

            if (streamingMode) {
                pendingFrames.push_back(std::move(generatedLatent));
                if ((int)pendingFrames.size() >= framesUntilFlush) {
                    flushChunk();
                    framesUntilFlush = std::min(chunkSize, framesUntilFlush * 2);
                }
            }
            else {
                collectedFrames.push_back(std::move(generatedLatent));
            }

            if (isEos && eosCountdown < 0) eosCountdown = framesAfterEos;
            if (eosCountdown >= 0) {
                if (eosCountdown == 0) break;
                eosCountdown--;
            }
        }

        threadContext.num_threads = threadsForPrefill; // restore for Mimi CNN decode

        if (streamingMode) {
            flushChunk(); // deliver any frames left over after the last full chunk
        }
        else {
            outputPcm = mimi_decode(collectedFrames, config, modelWeights);
            resample(outputPcm);
        }

        release_kv(std::move(kvHolder));

        if (listener) {
            auto t_end = std::chrono::steady_clock::now();
            SynthesisStats stats;
            stats.synthesis_ms = std::chrono::duration<float, std::milli>(t_end - t_start).count();
            stats.total_samples = streamingMode ? totalStreamingSamples : (int)outputPcm.size();
            stats.duration_sec = stats.total_samples > 0
                ? (float)stats.total_samples / config.mimi.sample_rate : 0.f;
            stats.rtf = stats.duration_sec > 0
                ? stats.synthesis_ms / (stats.duration_sec * 1000.f) : 0.f;
            listener->onFinish(id, streamingMode ? std::vector<float>{} : outputPcm, stats);
        }

        return outputPcm;
    }

    // ---------------------------------------------------------------------------
    // Voice loading
    // ---------------------------------------------------------------------------
    void PhononTTS::PhononTTSImpl::load_voice(const std::string& path) {
        const int modelDim = cfg.flow_lm.d_model;
        std::cerr << "[phonon] Loading voice: " << path << "\n";
        auto vt = safetensors::load_voice_tensor(path);

        std::vector<int64_t> dims = vt.shape;
        if (dims.size() == 3 && dims[0] == 1) dims.erase(dims.begin());
        if (dims.size() != 2)
            throw std::runtime_error("Voice tensor '" + vt.name + "' must be 2D or [1, ., .]: " + path);

        if (vt.name == "speaker_wavs") {
            // [C, T] speaker-Mimi latents → [T, C] → speaker_wavs.output_proj → [T, d_model].
            // Unprojected latents make the model babble or stop at once, so no fallback.
            if (!weights.spk_wavs_proj)
                throw std::runtime_error("Voice " + path + " holds speaker_wavs latents but the "
                    "checkpoint has no speaker_wavs.output_proj");
            const int channels = (int)dims[0], frames = (int)dims[1];
            if (channels != (int)weights.spk_wavs_proj->ne[0])
                throw std::runtime_error("Voice " + path + " has " + std::to_string(channels) +
                    " latent channels, the checkpoint expects " +
                    std::to_string(weights.spk_wavs_proj->ne[0]));
            std::vector<float> transposed((size_t)frames * channels);
            for (int c = 0; c < channels; c++)
                for (int t = 0; t < frames; t++)
                    transposed[(size_t)t * channels + c] = vt.data[(size_t)c * frames + t];
            weights.voice_emb.resize((size_t)frames * modelDim);
            linear_ggml(weights.spk_wavs_proj, transposed.data(), frames, channels, modelDim,
                nullptr, weights.voice_emb.data());
            weights.voice_len = frames;
        }
        else {
            if ((int)dims[1] != modelDim)
                throw std::runtime_error("Voice " + path + " is " + std::to_string(dims[1]) +
                    " wide, the model is " + std::to_string(modelDim));
            weights.voice_emb = std::move(vt.data);
            weights.voice_len = (int)dims[0];
        }
    }

    // Prefill the voice prefix once and keep its KV.  The voice embedding is fixed and
    // always occupies positions 0..voice_len, so its (RoPE'd) K/V are identical for every
    // synthesis call.  Per call we memcpy these in and prefill only the text tokens.
    void PhononTTS::PhononTTSImpl::prefill_voice() {
        const auto& flowConfig = cfg.flow_lm;
        const int voiceLen = weights.voice_len;
        voice_kv.ensure(flowConfig.num_layers, flowConfig.num_heads,
            voiceLen, flowConfig.d_model / flowConfig.num_heads);
        std::vector<float> hidden(weights.voice_emb);
        for (int l = 0; l < flowConfig.num_layers; l++)
            flow_lm_layer(hidden.data(), voiceLen, cfg, weights.flow_layers[l],
                voice_kv.kl(l), voice_kv.vl(l), 0, voiceLen);
    }

    // ---------------------------------------------------------------------------
    // PhononTTS public interface
    // ---------------------------------------------------------------------------

    PhononTTS::PhononTTS(const Paths& paths, const Config& cfg_in)
        : _ttsImpl(std::make_unique<PhononTTSImpl>())
    {
        PhononTTSImpl* m = _ttsImpl.get();
        m->cfg = cfg_in;
        if (!paths.config_json.empty()) {
            try { m->cfg = parse_config_impl(paths.config_json); }
            catch (const std::exception& e) {
                std::cerr << "[phonon] Config warn: " << e.what() << "\n";
            }
        }

        m->num_threads = std::max(1, (int)std::thread::hardware_concurrency());
        ThreadContext::current().num_threads = m->num_threads;

        std::cerr << "[phonon] Loading GGUF: " << paths.model_gguf << "\n";
        struct gguf_init_params gp = {/*.no_alloc=*/false, /*.ctx=*/&m->ctx_weights };
        m->gguf_ctx = gguf_init_from_file(paths.model_gguf.c_str(), gp);
        if (!m->gguf_ctx)
            throw std::runtime_error("Failed to load GGUF: " + paths.model_gguf);

        m->load_weights(paths);

        // The flow net runs a single Euler step, so it always conditions on the
        // average of the t=0 and t=1 timestep embeddings — a constant.  Compute
        // it once here instead of re-deriving it for every generated frame.
        {
            const int flowDim = m->cfg.flow_lm.flow_dim;
            std::vector<float> t0(flowDim), t1(flowDim);
            embed_timestep(0.f, m->weights.time_embeds[0], flowDim, t0.data());
            embed_timestep(1.f, m->weights.time_embeds[1], flowDim, t1.data());
            m->weights.time_embed_avg.resize(flowDim);
            for (int i = 0; i < flowDim; i++)
                m->weights.time_embed_avg[i] = (t0[i] + t1[i]) * 0.5f;
        }

        m->prefill_voice();

        std::cerr << "[phonon] Model loaded. Voice=" << m->weights.voice_len
            << " tokens. Threads=" << m->num_threads
            << (m->cfg.gelu_erf ? " GELU=erf" : " GELU=tanh")
            << (m->weights.dur_out_w ? " duration_delta=yes" : "") << "\n";
    }

    void PhononTTS::setVoice(const std::string& voice_path) {
        PhononTTSImpl* m = _ttsImpl.get();
        auto& threadContext = ThreadContext::current();
        if (threadContext.num_threads == 0) threadContext.num_threads = m->num_threads;
        m->load_voice(voice_path);
        m->prefill_voice();
    }

    PhononTTS::~PhononTTS() = default;
    PhononTTS::PhononTTS(PhononTTS&&) noexcept = default;
    PhononTTS& PhononTTS::operator=(PhononTTS&&) noexcept = default;

    std::vector<float> PhononTTS::synthesize(const std::string& text, const SynthOpts& opts) {
        PhononTTSImpl* m = _ttsImpl.get();
        const StreamId id = m->_next_stream_id.fetch_add(1, std::memory_order_relaxed);
        return m->run_synthesis(id, text, opts, nullptr, nullptr);
    }

    StreamId PhononTTS::synthesizeAsync(const std::string& text,
        const SynthOpts& opts,
        SynthesisListener& listener) {
        PhononTTSImpl* m = _ttsImpl.get();
        const StreamId id = m->_next_stream_id.fetch_add(1, std::memory_order_relaxed);

        auto state = std::make_unique<PhononTTSImpl::StreamState>();
        PhononTTSImpl::StreamState* state_ptr = state.get();

        // Start thread before inserting into the map so the thread field is fully
        // constructed before any other thread can see the StreamState entry.
        state->thread = std::thread([m, id, text, opts, &listener, state_ptr]() {
            // An exception escaping a std::thread terminates the process.
            try {
                m->run_synthesis(id, text, opts, &listener, &state_ptr->cancelled);
            }
            catch (const std::exception& e) {
                listener.onError(id, e.what());
            }
            });

        {
            std::lock_guard<std::mutex> lock(m->_streams_mutex);
            m->_active_streams.emplace(id, std::move(state));
        }
        return id;
    }

    void PhononTTS::cancelStream(StreamId id) {
        std::lock_guard<std::mutex> lock(_ttsImpl->_streams_mutex);
        auto it = _ttsImpl->_active_streams.find(id);
        if (it != _ttsImpl->_active_streams.end())
            it->second->cancelled.store(true, std::memory_order_relaxed);
    }

    void PhononTTS::waitForStream(StreamId id) {
        std::unique_ptr<PhononTTSImpl::StreamState> state;
        {
            std::lock_guard<std::mutex> lock(_ttsImpl->_streams_mutex);
            auto it = _ttsImpl->_active_streams.find(id);
            if (it == _ttsImpl->_active_streams.end()) return;
            state = std::move(it->second);
            _ttsImpl->_active_streams.erase(it);
        }
        if (state && state->thread.joinable())
            state->thread.join();
    }

    int PhononTTS::sampleRate() const noexcept {
        return _ttsImpl->cfg.mimi.sample_rate;
    }

    double PhononTTS::frameRate() const noexcept {
        return _ttsImpl->cfg.mimi.frame_rate;
    }

    const text::Tokenizer& PhononTTS::tokenizer() const noexcept {
        return _ttsImpl->textTokenizer;
    }

    bool PhononTTS::writeWav(const std::string& path,
        const std::vector<float>& pcm, int sample_rate) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        const int n = (int)pcm.size();
        const int data_bytes = n * 2;
        auto w2 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
        auto w4 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
        f.write("RIFF", 4); w4(36 + data_bytes);
        f.write("WAVE", 4); f.write("fmt ", 4);
        w4(16); w2(1); w2(1); w4(sample_rate); w4(sample_rate * 2); w2(2); w2(16);
        f.write("data", 4); w4(data_bytes);
        for (float s : pcm) {
            float  c = std::max(-1.f, std::min(1.f, s));
            int16_t v = (int16_t)(c * 32767.f);
            f.write(reinterpret_cast<char*>(&v), 2);
        }
        return true;
    }

    Config PhononTTS::parseConfig(const std::string& json_path) {
        return parse_config_impl(json_path);
    }

} // namespace gradium::tts
