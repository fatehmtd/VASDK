// mimi.cpp — Mimi neural codec decoder.
//
// Pipeline (per chunk of N latent frames):
//   1. Denormalise latents using per-dimension mean/std from the model.
//   2. Quantizer output projection: [N, ldim] → [N, dimension]  (k=1 conv as matmul).
//   3. Depthwise upsample ×16: produces [N×16, dimension] via convolution.
//   4. 2-layer causal transformer with sliding-window attention and LayerScale.
//   5. CNN decoder stack: ×6, ×5, ×4 transposed-conv stages → 1920 samples/frame.
//
// Decoding is stateful (MimiStreamState): every stage carries its exact
// left-context across calls, so feeding a stream chunk-by-chunk produces
// bit-identical PCM to decoding all frames at once — no overlap re-decoding.

#include "mimi.h"
#include "ops.h"  // conv::conv1d, conv::conv_transpose1d — GGML-backed

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace gradium::tts {

// ---------------------------------------------------------------------------
// Mimi transformer — 2 causal layers, LayerScale, sliding-window attention.
// ---------------------------------------------------------------------------
static void mimi_transformer_fwd(float* x, int T, const Config& cfg,
                                  const ModelWeights& weights, KVCache& kv,
                                  int past, int pos_base) {
    const auto& mc = cfg.mimi;
    int d    = mc.transformer_d_model;
    int d_ff = d * 4;
    int nh   = mc.transformer_num_heads;
    float mp = mc.transformer_max_period;

    for (int l = 0; l < mc.transformer_num_layers; l++) {
        const auto& lw = weights.mimi_tr_layers[l];
        float* kc_l = kv.kl(l);
        float* vc_l = kv.vl(l);

        std::vector<float> buf(T*d), attn(T*d), ffn_hidden(T*d_ff), ffn_out(T*d);

        // Pre-norm → attention (with LayerScale) → residual
        for (int s = 0; s < T; s++)
            math::layer_norm(x+s*d, d, lw.norm1_w.data(), lw.norm1_b.data(), 1e-5f, buf.data()+s*d);
        mha_fwd(buf.data(), T, d, nh, lw.in_proj, lw.out_proj,
                kc_l, vc_l, past, kv.ctx_len, mp, attn.data(),
                mc.transformer_context, pos_base);
        for (int i = 0; i < T*d; i++) x[i] += lw.ls1[i%d] * attn[i];

        // Pre-norm → GELU FFN (with LayerScale) → residual
        for (int s = 0; s < T; s++)
            math::layer_norm(x+s*d, d, lw.norm2_w.data(), lw.norm2_b.data(), 1e-5f, buf.data()+s*d);
        linear_ggml(lw.linear1, buf.data(), T, d, d_ff, nullptr, ffn_hidden.data());
        math::gelu_inplace(ffn_hidden.data(), ffn_hidden.size(), cfg.gelu_erf);
        linear_ggml(lw.linear2, ffn_hidden.data(), T, d_ff, d, nullptr, ffn_out.data());
        for (int i = 0; i < T*d; i++) x[i] += lw.ls2[i%d] * ffn_out[i];
    }
}

// ---------------------------------------------------------------------------
// Streaming Mimi decode  latents[N, ldim] → PCM[N × 1920], carrying state
// ---------------------------------------------------------------------------
std::vector<float> mimi_decode_stream(const std::vector<std::vector<float>>& latents,
                                      const Config& cfg,
                                      const ModelWeights& weights,
                                      MimiStreamState& st) {
    const auto& mc = cfg.mimi;
    const int ld = cfg.flow_lm.ldim;
    const int md = mc.dimension;
    const int N  = static_cast<int>(latents.size());
    if (N == 0) return {};

    if (!st.initialized) {
        st.up_carry.assign(16 * md, 0.f);
        st.c0_prev.assign(6 * md, 0.f);
        st.t2_carry.assign(6 * 256, 0.f);
        st.r3_prev.assign(2 * 256, 0.f);
        st.t5_carry.assign(5 * 128, 0.f);
        st.r6_prev.assign(2 * 128, 0.f);
        st.t8_carry.assign(4 * 64, 0.f);
        st.r9_prev.assign(2 * 64, 0.f);
        st.c11_prev.assign(2 * 64, 0.f);
        st.initialized = true;
    }

    // 1. Denormalise: z_norm = z * emb_std + emb_mean
    std::vector<float> flat(N * ld);
    for (int t = 0; t < N; t++)
        for (int i = 0; i < ld; i++)
            flat[t*ld + i] = latents[t][i] * weights.emb_std[i] + weights.emb_mean[i];

    // 2. Quantizer output projection (k=1 conv = matmul): [N, ld] → [N, md]
    // mimi_qout_w is stored [md, ld] with ld innermost = ne[0]=ld, ne[1]=md for GGML.
    std::vector<float> projected(N * md);
    {
        constexpr size_t kHdrSz = 4 * 1024;
        alignas(GGML_MEM_ALIGN) uint8_t hdr_buf[kHdrSz];
        auto& tc = ThreadContext::current();
        ggml_context* hctx = ggml_init({kHdrSz, hdr_buf, true});
        ggml_tensor* W_t   = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, ld, md);
        W_t->data          = const_cast<float*>(weights.mimi_qout_w.data());
        ggml_tensor* x_t   = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, ld, N);
        x_t->data          = flat.data();

        size_t cmp_sz = (size_t)N * md * sizeof(float) + 2ul * 1024 * 1024;
        if (tc.ggml_ctx_buf.size() < cmp_sz) tc.ggml_ctx_buf.resize(cmp_sz);
        ggml_context* ctx = ggml_init({cmp_sz, tc.ggml_ctx_buf.data(), false});
        ggml_tensor* res  = ggml_mul_mat(ctx, W_t, x_t);
        ggml_cgraph* gf   = ggml_new_graph(ctx);
        ggml_build_forward_expand(gf, res);
        ggml_cplan plan   = ggml_graph_plan(gf, tc.num_threads, nullptr);
        if (plan.work_size > tc.ggml_work_buf.size()) tc.ggml_work_buf.resize(plan.work_size);
        plan.work_data = plan.work_size > 0 ? tc.ggml_work_buf.data() : nullptr;
        ggml_graph_compute(gf, &plan);
        memcpy(projected.data(), res->data, (size_t)N * md * sizeof(float));
        ggml_free(ctx);
        ggml_free(hctx);
    }

    // 3. Depthwise upsample ×16 (k=32, stride=16).
    // weights.mimi_up_w is [k=32, md] after rearrangement at load time.
    // raw[t*stride + k, :] += projected[t, :] ⊙ W[k, :]  (element-wise per channel)
    // The 16 raw rows beyond T_up overlap the next chunk → carried in st.up_carry.
    constexpr int up_kernel = 32, up_stride = 16;
    const int T_up  = N * up_stride;
    const int T_raw = (N-1)*up_stride + up_kernel;
    std::vector<float> raw_up(T_raw * md, 0.f);
    // The t-major scatter below accumulates older frames first, so the carry
    // (the previous chunk's last frame) is added before it — the same float
    // summation order as one-shot decoding (bit-exact chunking).
    for (int i = 0; i < up_stride * md; i++) raw_up[i] += st.up_carry[i];
    for (int t = 0; t < N; t++) {
        const float* prow = projected.data() + t * md;
        for (int k = 0; k < up_kernel; k++) {
            float*       rrow = raw_up.data() + (t*up_stride + k) * md;
            const float* wrow = weights.mimi_up_w.data() + k * md;
            for (int c = 0; c < md; c++) rrow[c] += prow[c] * wrow[c];
        }
    }
    memcpy(st.up_carry.data(), raw_up.data() + (size_t)T_up * md,
        (size_t)up_stride * md * sizeof(float));
    std::vector<float> upsampled(raw_up.begin(), raw_up.begin() + (size_t)T_up * md);

    // 4. Mimi transformer (causal, sliding window) with persistent KV.
    {
        const int window = mc.transformer_context;
        const int hd = mc.transformer_d_model / mc.transformer_num_heads;

        // Size for the largest chunk this stream can see (the geometry must
        // not change once rows are cached), so ensure() is a no-op afterwards.
        const int maxT_up = std::max(T_up, st.max_chunk_frames * up_stride);
        st.kv.ensure(mc.transformer_num_layers, mc.transformer_num_heads,
            window + maxT_up + up_stride, hd);

        // Rebase when the cache is full: keep the last `window` rows (the only
        // ones future queries can attend to) and shift positions so the cache
        // stays bounded on long streams.  RoPE uses pos_base + cache index, so
        // relative offsets are preserved exactly.
        if (st.past + T_up > st.kv.ctx_len) {
            const int keep = std::min(st.past, window);
            const int shift = st.past - keep;
            for (int l = 0; l < mc.transformer_num_layers; l++) {
                for (int h = 0; h < mc.transformer_num_heads; h++) {
                    float* kh = st.kv.kl(l) + (size_t)h * st.kv.ctx_len * hd;
                    float* vh = st.kv.vl(l) + (size_t)h * st.kv.ctx_len * hd;
                    memmove(kh, kh + (size_t)shift * hd, (size_t)keep * hd * sizeof(float));
                    memmove(vh, vh + (size_t)shift * hd, (size_t)keep * hd * sizeof(float));
                }
            }
            st.pos_base += shift;
            st.past = keep;
        }

        mimi_transformer_fwd(upsampled.data(), T_up, cfg, weights, st.kv,
            st.past, st.pos_base);
        st.past += T_up;
    }

    // 5. CNN decoder stack — total upsampling: 16 (dw) × 6 × 5 × 4 = 1920 samp/frame
    // Causal convs prepend the previous chunk's tail rows (zeros on the first
    // chunk — identical to the pad_left of one-shot decoding).

    // d_c0: Conv1d k=7  md→md  (ELU activation); left context = 6 rows.
    std::vector<float> stage0(T_up * md);
    {
        std::vector<float> ext((size_t)(6 + T_up) * md);
        memcpy(ext.data(), st.c0_prev.data(), (size_t)6 * md * sizeof(float));
        memcpy(ext.data() + (size_t)6 * md, upsampled.data(), (size_t)T_up * md * sizeof(float));
        memcpy(st.c0_prev.data(), ext.data() + (size_t)T_up * md, (size_t)6 * md * sizeof(float));
        conv::conv1d(ext.data(), 6 + T_up, md,
                     weights.d_c0_w.data(), md, 7, weights.d_c0_b.data(),
                     stage0.data(), 1, 1, 0, 0);
    }
    for (auto& v : stage0) v = conv::elu(v);

    // d_t2: ConvTranspose1d k=12 stride=6  md→256, then ResBlock + ELU
    const int T2 = T_up * 6;
    std::vector<float> stage2(T2 * 256);
    conv::conv_transpose1d(stage0.data(), T_up, md,
                           weights.d_t2_w.data(), 256, 12, weights.d_t2_b.data(),
                           stage2.data(), 6, st.t2_carry.data());
    conv::res_block(stage2.data(), T2, 256,
                    weights.d_r3a_w.data(), weights.d_r3a_b.data(),
                    weights.d_r3b_w.data(), weights.d_r3b_b.data(),
                    1, st.r3_prev.data());
    for (auto& v : stage2) v = conv::elu(v);

    // d_t5: ConvTranspose1d k=10 stride=5  256→128, then ResBlock + ELU
    const int T5 = T2 * 5;
    std::vector<float> stage5(T5 * 128);
    conv::conv_transpose1d(stage2.data(), T2, 256,
                           weights.d_t5_w.data(), 128, 10, weights.d_t5_b.data(),
                           stage5.data(), 5, st.t5_carry.data());
    conv::res_block(stage5.data(), T5, 128,
                    weights.d_r6a_w.data(), weights.d_r6a_b.data(),
                    weights.d_r6b_w.data(), weights.d_r6b_b.data(),
                    1, st.r6_prev.data());
    for (auto& v : stage5) v = conv::elu(v);

    // d_t8: ConvTranspose1d k=8 stride=4  128→64, then ResBlock + ELU
    const int T8 = T5 * 4;
    std::vector<float> stage8(T8 * 64);
    conv::conv_transpose1d(stage5.data(), T5, 128,
                           weights.d_t8_w.data(), 64, 8, weights.d_t8_b.data(),
                           stage8.data(), 4, st.t8_carry.data());
    conv::res_block(stage8.data(), T8, 64,
                    weights.d_r9a_w.data(), weights.d_r9a_b.data(),
                    weights.d_r9b_w.data(), weights.d_r9b_b.data(),
                    1, st.r9_prev.data());
    for (auto& v : stage8) v = conv::elu(v);

    // d_c11: Conv1d k=3  64→1; left context = 2 rows. No final activation, as in the
    // reference SEANet decoder.
    std::vector<float> pcm(T8);
    {
        std::vector<float> ext((size_t)(2 + T8) * 64);
        memcpy(ext.data(), st.c11_prev.data(), (size_t)2 * 64 * sizeof(float));
        memcpy(ext.data() + (size_t)2 * 64, stage8.data(), (size_t)T8 * 64 * sizeof(float));
        memcpy(st.c11_prev.data(), ext.data() + (size_t)T8 * 64, (size_t)2 * 64 * sizeof(float));
        conv::conv1d(ext.data(), 2 + T8, 64,
                     weights.d_c11_w.data(), 1, 3, weights.d_c11_b.data(),
                     pcm.data(), 1, 1, 0, 0);
    }

    return pcm;
}

// ---------------------------------------------------------------------------
// One-shot decode — a stream of exactly one chunk.
// ---------------------------------------------------------------------------
std::vector<float> mimi_decode(const std::vector<std::vector<float>>& latents,
                               const Config& cfg,
                               const ModelWeights& weights) {
    MimiStreamState state;
    return mimi_decode_stream(latents, cfg, weights, state);
}

} // namespace gradium::tts
