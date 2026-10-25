#pragma once
// model_types.h — Internal weight structs, KV-cache, per-thread GGML scratch
// buffers, and declarations for the shared low-level GGML helpers.
// Included by phonon.cpp, mimi.cpp, and any future inference modules.

#include "phonon.h"

#include <ggml.h>
#include <gguf.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace gradium::tts {

// ---------------------------------------------------------------------------
// Per-thread GGML context.
//
// Each synthesis thread has its own scratch buffers and thread count so
// concurrent calls never share mutable state.  Access via current().
// ---------------------------------------------------------------------------
struct ThreadContext {
    int num_threads = 0;
    std::vector<uint8_t> ggml_ctx_buf;
    std::vector<uint8_t> ggml_work_buf;

    static ThreadContext& current() noexcept {
        thread_local ThreadContext instance;
        return instance;
    }

private:
    ThreadContext() = default;
};

// ---------------------------------------------------------------------------
// Weight structs
// ---------------------------------------------------------------------------

// Weights for one transformer layer (used by Flow-LM and Mimi transformer).
struct LayerW {
    ggml_tensor* in_proj  = nullptr;
    ggml_tensor* out_proj = nullptr;
    ggml_tensor* linear1  = nullptr;
    ggml_tensor* linear2  = nullptr;
    std::vector<float> norm1_w, norm1_b;
    std::vector<float> norm2_w, norm2_b;
    std::vector<float> ls1, ls2; // LayerScale coefficients (Mimi transformer only)
};

// Weights for one flow-network residual block (adaptive LayerNorm + MLP).
struct FlowResBlockW {
    ggml_tensor* mlp0_w = nullptr;
    ggml_tensor* mlp2_w = nullptr;
    ggml_tensor* ada_w  = nullptr; // adaLN modulation linear
    std::vector<float> mlp0_b, mlp2_b, ada_b;
    std::vector<float> in_ln_w, in_ln_b;
};

// Weights for the timestep embedder (sinusoidal → MLP → RMSNorm).
struct TimeEmbedW {
    ggml_tensor* mlp0_w = nullptr;
    ggml_tensor* mlp2_w = nullptr;
    std::vector<float> freqs, mlp0_b, mlp2_b, alpha;
    int n_freqs = 0;
};

// All model weights, organised by sub-module.
struct ModelWeights {
    // ---- Flow-LM transformer ----
    std::vector<LayerW> flow_layers;
    std::vector<float>  out_norm_w, out_norm_b;
    std::vector<float>  out_eos_w;   // dequantised — used as a plain dot product per frame
    std::vector<float>  out_eos_b;
    std::vector<float>  bos_emb, emb_mean, emb_std;
    ggml_tensor*        input_linear = nullptr;
    std::vector<float>  txt_embed;   // [vocab, d_model]
    std::vector<float>  spk_lut;     // [n_speakers, lut_dim]
    int                 spk_lut_d = 16;
    ggml_tensor*        spk_out_w = nullptr;
    // speaker_wavs.output_proj: speaker-Mimi latents [T, C] → voice embedding [T, d_model].
    // Absent on checkpoints whose voices are stored pre-projected ("emb").
    ggml_tensor*        spk_wavs_proj = nullptr;
    // duration_delta.output_proj: sinusoidal embedding [dur_dim] → d_model. Optional.
    ggml_tensor*        dur_out_w = nullptr;
    int                 dur_dim = 0;

    // ---- Flow network ----
    ggml_tensor*             fp_in_w = nullptr;
    ggml_tensor*             fp_ce_w = nullptr;
    std::vector<float>       fp_in_b, fp_ce_b;
    std::vector<TimeEmbedW>  time_embeds;
    std::vector<FlowResBlockW> res_blocks;
    ggml_tensor*             fp_fl_ada_w = nullptr;
    ggml_tensor*             fp_fl_lin_w = nullptr;
    std::vector<float>       fp_fl_ada_b, fp_fl_lin_b;
    // (time_embed(0) + time_embed(1)) / 2 — input-independent for the single
    // Euler step, so it is precomputed once at load time.
    std::vector<float>       time_embed_avg;

    // ---- Mimi codec ----
    std::vector<float>  mimi_qout_w; // [dimension, ldim] F32 — k=1 conv as matmul
    std::vector<float>  mimi_up_w;   // [k=32, dimension] after rearrangement
    std::vector<LayerW> mimi_tr_layers;

    // ---- Mimi CNN decoder (always F32 after dequant + layout rearrangement) ----
    std::vector<float> d_c0_w,  d_c0_b;
    std::vector<float> d_t2_w,  d_t2_b;
    std::vector<float> d_r3a_w, d_r3a_b, d_r3b_w, d_r3b_b;
    std::vector<float> d_t5_w,  d_t5_b;
    std::vector<float> d_r6a_w, d_r6a_b, d_r6b_w, d_r6b_b;
    std::vector<float> d_t8_w,  d_t8_b;
    std::vector<float> d_r9a_w, d_r9a_b, d_r9b_w, d_r9b_b;
    std::vector<float> d_c11_w, d_c11_b;

    // ---- Voice embedding ----
    std::vector<float> voice_emb;
    int voice_len = 0;
};

// ---------------------------------------------------------------------------
// KV-cache — pre-allocated storage for all transformer layers.
// kl(l) / vl(l) return the contiguous key/value slice for layer l.
// ---------------------------------------------------------------------------
struct KVCache {
    int nl = 0, nh = 0, ctx_len = 0, hd = 0;
    std::unique_ptr<float[]> k, v;

    KVCache() = default;
    KVCache(int nl_, int nh_, int ctx, int hd_) { ensure(nl_, nh_, ctx, hd_); }

    // (Re)shape the cache, growing storage only when needed.  Contents are
    // never zeroed: every slot is written by prefill/decode before any read
    // (attention only reads positions < past + seq).
    void ensure(int nl_, int nh_, int ctx, int hd_) {
        if (nl_ == nl && nh_ == nh && hd_ == hd && ctx <= ctx_len) return;
        const size_t need = (size_t)nl_ * nh_ * ctx * hd_;
        if (need > _cap) {
            k.reset(new float[need]);
            v.reset(new float[need]);
            _cap = need;
        }
        nl = nl_; nh = nh_; ctx_len = ctx; hd = hd_;
    }

    float* kl(int l) { return k.get() + (size_t)l*nh*ctx_len*hd; }
    float* vl(int l) { return v.get() + (size_t)l*nh*ctx_len*hd; }

private:
    size_t _cap = 0; // allocated floats in each of k / v
};

// ---------------------------------------------------------------------------
// GGML helpers — defined in phonon.cpp, used by mimi.cpp too.
// ---------------------------------------------------------------------------

// y[seq, out] = x[seq, in] @ W^T + bias.  W may be any GGML dtype.
void linear_ggml(ggml_tensor* W,
                 const float* x, int seq, int in, int out,
                 const float* bias,
                 float* y);

// Dequantise any GGML tensor to a flat F32 vector.
void dequant_f32(const ggml_tensor* t, std::vector<float>& out);

// Multi-head causal attention with RoPE and KV-cache.
// window=0 → full causal; window>0 → sliding window of that size.
// pos_base: absolute position of cache slot 0 — RoPE positions are
// pos_base + cache index (nonzero only for rebased streaming caches).
void mha_fwd(const float* x, int seq, int hiddenDim, int num_heads,
             ggml_tensor* W_in, ggml_tensor* W_out,
             float* keyCache, float* valueCache,
             int past, int max_ctx, float max_period,
             float* out, int window = 0, int pos_base = 0);

} // namespace gradium::tts
