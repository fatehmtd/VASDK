#pragma once
// ops.h — Header-only convolution and neural-network math helpers.
// Split into two namespaces:
//   conv:: — im2col+ggml_mul_mat conv1d, batched-matmul conv_transpose1d,
//            residual block, weight layout rearrangement utilities.
//   math:: — Layer/RMS normalisation, rotary embeddings, softmax, sinusoidal
//            embedding, and element-wise activations (silu, elu).
//
// Both conv1d and conv_transpose1d use ggml_mul_mat internally so they run
// on GGML's AVX2/AVX512 GEMM kernels (and any GPU backend when enabled).

#include "model_types.h"

#include <ggml-cpu.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// conv — 1D convolution primitives
//
// Weight layout CONTRACT (established at load time via rearrange_*):
//   conv1d           W: [oc, k, ic]   (rearranged from PyTorch [oc, ic, k])
//   conv_transpose1d W: [k, oc, ic]   (rearranged from PyTorch [ic, oc, k])
//                       ic innermost so each W_k slice is [oc, ic] with
//                       ic fastest — matches ggml_mul_mat ne[0]=ic.
//   depthwise_up_w:  [k, c]           (rearranged from [c, k])
//
// im2col layout: col[t, k*in_ch + ic]  (k outer, ic inner — memcpy-friendly)
// ---------------------------------------------------------------------------
namespace conv {

    // Rearrange Conv1d weight [oc, ic, k] → [oc, k, ic] in-place.
    inline void rearrange_conv1d_w(std::vector<float>& W, int oc, int ic, int k) {
        std::vector<float> tmp(W.size());
        for (int o = 0; o < oc; o++)
            for (int ki = 0; ki < k; ki++)
                for (int c = 0; c < ic; c++)
                    tmp[(size_t)o * ic * k + ki * ic + c] = W[(size_t)o * ic * k + c * k + ki];
        W = std::move(tmp);
    }

    // Rearrange ConvTranspose1d weight [ic, oc, k] → [k, oc, ic] in-place.
    // ic is the fastest-varying dimension so that each W_k slice is laid out as
    // [oc, ic] with ic innermost — the layout expected by ggml_mul_mat (ne[0]=ic).
    inline void rearrange_convtr1d_w(std::vector<float>& W, int ic, int oc, int k) {
        std::vector<float> tmp(W.size());
        for (int ki = 0; ki < k; ki++)
            for (int o = 0; o < oc; o++)
                for (int c = 0; c < ic; c++)
                    tmp[(size_t)ki * oc * ic + o * ic + c] = W[(size_t)c * oc * k + o * k + ki];
        W = std::move(tmp);
    }

    // Rearrange depthwise upsample weight [channels, k] → [k, channels] in-place.
    inline void rearrange_dw_w(std::vector<float>& W, int ch, int k) {
        std::vector<float> tmp(W.size());
        for (int ki = 0; ki < k; ki++)
            for (int c = 0; c < ch; c++)
                tmp[ki * ch + c] = W[c * k + ki];
        W = std::move(tmp);
    }

    inline float elu(float x) { return x >= 0.f ? x : std::exp(x) - 1.f; }

    // Per-thread im2col scratch — reused across calls to avoid repeated allocation.
    struct ConvScratch {
        std::vector<float> im2col_buf;

        static ConvScratch& current() noexcept {
            thread_local ConvScratch instance;
            return instance;
        }

        private:
        ConvScratch() = default;
    };

    // ---------------------------------------------------------------------------
    // conv1d — im2col then ggml_mul_mat for the GEMM.
    // W must be in [oc, k, ic] layout (ne[0]=ic innermost → GGML ne[0]=col_w).
    // ---------------------------------------------------------------------------
    inline void conv1d(const float* x, int T, int in_ch,
        const float* W, int out_ch, int kernel,
        const float* bias, float* y,
        int stride = 1, int dilation = 1,
        int pad_left = 0, int pad_right = 0) {
        int T_out = (T + pad_left + pad_right - dilation * (kernel - 1) - 1) / stride + 1;
        int col_w = kernel * in_ch;

        // im2col → col[T_out, col_w]; reuse thread-local buffer.
        auto& scratch = ConvScratch::current().im2col_buf;
        size_t col_sz = (size_t)T_out * col_w;
        if (scratch.size() < col_sz) scratch.resize(col_sz);
        float* col = scratch.data();
        memset(col, 0, col_sz * sizeof(float));
        for (int t = 0; t < T_out; t++) {
            float* crow = col + (size_t)t * col_w;
            for (int k = 0; k < kernel; k++) {
                int ti = t * stride - pad_left + k * dilation;
                if (ti >= 0 && ti < T)
                    memcpy(crow + k * in_ch, x + ti * in_ch, in_ch * sizeof(float));
            }
        }

        // Wrap W and col as no-alloc tensor views (no data copy for weights).
        auto& tc = gradium::tts::ThreadContext::current();
        uint8_t hdr_buf[4096];
        ggml_context* hctx = ggml_init({ sizeof(hdr_buf), hdr_buf, /*no_alloc=*/true });
        ggml_tensor* W_t = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, col_w, out_ch);
        W_t->data = const_cast<float*>(W);
        ggml_tensor* col_t = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, col_w, T_out);
        col_t->data = col;

        size_t cmp_sz = (size_t)T_out * out_ch * sizeof(float) + 4ul * 1024 * 1024;
        if (tc.ggml_ctx_buf.size() < cmp_sz) tc.ggml_ctx_buf.resize(cmp_sz);
        ggml_context* ctx = ggml_init({ cmp_sz, tc.ggml_ctx_buf.data(), false });

        ggml_tensor* res = ggml_mul_mat(ctx, W_t, col_t);  // result: [T_out, out_ch]
        ggml_cgraph* gf = ggml_new_graph(ctx);
        ggml_build_forward_expand(gf, res);

        ggml_cplan plan = ggml_graph_plan(gf, tc.num_threads, nullptr);
        if (plan.work_size > tc.ggml_work_buf.size()) tc.ggml_work_buf.resize(plan.work_size);
        plan.work_data = plan.work_size > 0 ? tc.ggml_work_buf.data() : nullptr;
        ggml_graph_compute(gf, &plan);

        // res.ne = [out_ch, T_out]; data[t*out_ch + o] matches y layout.
        memcpy(y, res->data, (size_t)T_out * out_ch * sizeof(float));
        if (bias)
            for (int t = 0; t < T_out; t++)
                for (int o = 0; o < out_ch; o++)
                    y[t * out_ch + o] += bias[o];

        ggml_free(ctx);
        ggml_free(hctx);
    }

    // ---------------------------------------------------------------------------
    // conv_transpose1d — all K kernel-position matmuls in a single GGML graph.
    // W must be in [k, oc, ic] layout (produced by rearrange_convtr1d_w).
    // Each W_k slice at W + k*out_ch*in_ch is [oc, ic] with ic innermost.
    //
    // carry (optional, (kernel-stride) × out_ch floats): streaming overlap.
    // On entry it is added into the first raw rows (contribution of the
    // previous chunk's tail); on exit it holds this chunk's pre-bias tail rows
    // beyond T*stride.  Pass nullptr for one-shot decoding.
    // ---------------------------------------------------------------------------
    inline void conv_transpose1d(const float* x, int T, int in_ch,
        const float* W, int out_ch, int kernel,
        const float* bias, float* y, int stride,
        float* carry = nullptr) {
        int T_raw = (T - 1) * stride + kernel;
        int T_out = T * stride;
        std::vector<float> raw((size_t)T_raw * out_ch, 0.f);

        auto& tc = gradium::tts::ThreadContext::current();

        // Header context: 1 shared x_t + K W_k tensors (no data allocation).
        size_t hdr_sz = (size_t)(kernel + 2) * ggml_tensor_overhead() + 1024;
        std::vector<uint8_t> hdr_buf(hdr_sz);
        ggml_context* hctx = ggml_init({ hdr_sz, hdr_buf.data(), /*no_alloc=*/true });

        ggml_tensor* x_t = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, in_ch, T);
        x_t->data = const_cast<float*>(x);

        // Compute context holds K result tensors [T, out_ch].
        size_t cmp_sz = (size_t)kernel * T * out_ch * sizeof(float) + 8ul * 1024 * 1024;
        if (tc.ggml_ctx_buf.size() < cmp_sz) tc.ggml_ctx_buf.resize(cmp_sz);
        ggml_context* ctx = ggml_init({ cmp_sz, tc.ggml_ctx_buf.data(), false });

        std::vector<ggml_tensor*> results(kernel);
        for (int k = 0; k < kernel; k++) {
            ggml_tensor* W_k = ggml_new_tensor_2d(hctx, GGML_TYPE_F32, in_ch, out_ch);
            W_k->data = const_cast<float*>(W + (size_t)k * out_ch * in_ch);
            results[k] = ggml_mul_mat(ctx, W_k, x_t);  // [T, out_ch]
        }

        ggml_cgraph* gf = ggml_new_graph(ctx);
        for (int k = 0; k < kernel; k++)
            ggml_build_forward_expand(gf, results[k]);

        ggml_cplan plan = ggml_graph_plan(gf, tc.num_threads, nullptr);
        if (plan.work_size > tc.ggml_work_buf.size()) tc.ggml_work_buf.resize(plan.work_size);
        plan.work_data = plan.work_size > 0 ? tc.ggml_work_buf.data() : nullptr;
        ggml_graph_compute(gf, &plan);

        // Scatter-add each kernel contribution at its stride offset.
        for (int k = 0; k < kernel; k++) {
            const float* rp = (const float*)results[k]->data;
            for (int t = 0; t < T; t++) {
                float* rrow = raw.data() + (size_t)(t * stride + k) * out_ch;
                const float* crow = rp + (size_t)t * out_ch;
                for (int o = 0; o < out_ch; o++) rrow[o] += crow[o];
            }
        }

        // Streaming overlap.  The k-major scatter above means a boundary row
        // accumulates its current-chunk (small-k) contribution before the
        // previous chunk's (large-k) one, so the carry is added after — the
        // same float summation order as one-shot decoding (bit-exact chunking).
        const int tail = kernel - stride; // raw rows beyond T_out
        if (carry) {
            for (int i = 0; i < tail * out_ch; i++) raw[i] += carry[i];
            memcpy(carry, raw.data() + (size_t)T_out * out_ch,
                (size_t)tail * out_ch * sizeof(float));
        }

        for (int t = 0; t < T_out; t++) {
            const float* rrow = raw.data() + t * out_ch;
            float* yrow = y + t * out_ch;
            for (int o = 0; o < out_ch; o++)
                yrow[o] = rrow[o] + (bias ? bias[o] : 0.f);
        }

        ggml_free(ctx);
        ggml_free(hctx);
    }

    // ---------------------------------------------------------------------------
    // ELU residual block: two conv1d layers with dilation, added back to input.
    // in_ch is the full channel count; intermediate is in_ch/2.
    // Both inner conv1d calls automatically use the GGML-backed implementation.
    //
    // carry (optional, 2*dilation × in_ch floats): streaming left-context for
    // the k=3 conv — holds the previous chunk's last ELU'd input rows and is
    // updated with this chunk's.  Must be zero-filled before the first chunk;
    // pass nullptr for one-shot decoding (zero padding instead).
    // ---------------------------------------------------------------------------
    inline void res_block(float* x, int T, int in_ch,
        const float* b1_w, const float* b1_b,
        const float* b3_w, const float* b3_b,
        int dilation = 1, float* carry = nullptr) {
        int out_ch = in_ch / 2;
        const int ctxRows = 2 * dilation;
        std::vector<float> h(T * out_ch);
        if (carry) {
            std::vector<float> xelu((size_t)(ctxRows + T) * in_ch);
            memcpy(xelu.data(), carry, (size_t)ctxRows * in_ch * sizeof(float));
            for (int i = 0; i < T * in_ch; i++) xelu[ctxRows * in_ch + i] = elu(x[i]);
            memcpy(carry, xelu.data() + (size_t)T * in_ch,
                (size_t)ctxRows * in_ch * sizeof(float));
            conv1d(xelu.data(), ctxRows + T, in_ch, b1_w, out_ch, 3, b1_b, h.data(),
                1, dilation, 0, 0);
        }
        else {
            std::vector<float> xelu((size_t)T * in_ch);
            for (int i = 0; i < T * in_ch; i++) xelu[i] = elu(x[i]);
            conv1d(xelu.data(), T, in_ch, b1_w, out_ch, 3, b1_b, h.data(),
                1, dilation, ctxRows, 0);
        }
        for (auto& v : h) v = elu(v);
        std::vector<float> out(T * in_ch);
        conv1d(h.data(), T, out_ch, b3_w, in_ch, 1, b3_b, out.data());
        for (int i = 0; i < T * in_ch; i++) x[i] += out[i];
    }

} // namespace conv

// ---------------------------------------------------------------------------
// math — Neural-network math primitives
// ---------------------------------------------------------------------------
namespace math {

    inline float silu(float x) { return x / (1.f + std::exp(-x)); }
    inline float elu(float x) { return x >= 0.f ? x : std::exp(x) - 1.f; }

    // GELU, in place: exact erf form (Phonon checkpoints) or the tanh approximation
    // (Pocket TTS ones).
    inline void gelu_inplace(float* v, size_t n, bool use_erf) {
        if (use_erf) {
            for (size_t i = 0; i < n; i++)
                v[i] = 0.5f * v[i] * (1.f + std::erf(v[i] * 0.70710678118f));
        }
        else {
            for (size_t i = 0; i < n; i++) {
                float t = std::tanh(0.7978845608f * (v[i] + 0.044715f * v[i] * v[i] * v[i]));
                v[i] = 0.5f * v[i] * (1.f + t);
            }
        }
    }

    // Standard LayerNorm.  w and b may be nullptr (identity scale/bias).
    inline void layer_norm(const float* x, int n, const float* w, const float* b,
        float eps, float* y) {
        float mean = 0.f;
        for (int i = 0; i < n; i++) mean += x[i];
        mean /= n;
        float var = 0.f;
        for (int i = 0; i < n; i++) { float d = x[i] - mean; var += d * d; }
        float inv = 1.f / std::sqrt(var / n + eps);
        for (int i = 0; i < n; i++)
            y[i] = (x[i] - mean) * inv * (w ? w[i] : 1.f) + (b ? b[i] : 0.f);
    }

    // RMS normalisation (no mean subtraction).
    inline void rms_norm(const float* x, int n, const float* w, float eps, float* y) {
        float ms = 0.f;
        for (int i = 0; i < n; i++) ms += x[i] * x[i];
        float inv = 1.f / std::sqrt(ms / n + eps);
        for (int i = 0; i < n; i++) y[i] = x[i] * inv * w[i];
    }

    // Cached RoPE inverse-frequency table.  Keyed by (head_dim, max_period);
    // in practice all transformers here use head_dim=64, max_period=10000, so
    // the table is computed once per thread and reused forever.
    inline const float* rope_inv_freqs(int head_dim, float max_period) {
        thread_local int cached_hd = 0;
        thread_local float cached_mp = 0.f;
        thread_local std::vector<float> table;
        const int hd = head_dim / 2;
        if (cached_hd != hd || cached_mp != max_period) {
            table.resize(hd);
            for (int i = 0; i < hd; i++)
                table[i] = 1.f / std::pow(max_period, (float)i / (float)hd);
            cached_hd = hd; cached_mp = max_period;
        }
        return table.data();
    }

    // Apply rotary positional embedding to a query or key vector in-place.
    inline void apply_rope(float* qk, int head_dim, int pos, float max_period) {
        int hd = head_dim / 2;
        const float* inv = rope_inv_freqs(head_dim, max_period);
        for (int i = 0; i < hd; i++) {
            float a = (float)pos * inv[i];
            float c = std::cos(a), s = std::sin(a);
            float x0 = qk[2 * i], x1 = qk[2 * i + 1];
            qk[2 * i] = x0 * c - x1 * s;
            qk[2 * i + 1] = x0 * s + x1 * c;
        }
    }

    // RoPE with a precomputed cos/sin table (c[i], s[i] for each rotation pair).
    inline void apply_rope_cached(float* qk, int half_dim,
        const float* c, const float* s) {
        for (int i = 0; i < half_dim; i++) {
            float x0 = qk[2 * i], x1 = qk[2 * i + 1];
            qk[2 * i] = x0 * c[i] - x1 * s[i];
            qk[2 * i + 1] = x0 * s[i] + x1 * c[i];
        }
    }

    // Numerically-stable softmax in-place over n elements.
    inline void softmax_inplace(float* x, int n) {
        float mx = *std::max_element(x, x + n);
        float sum = 0.f;
        for (int i = 0; i < n; i++) { x[i] = std::exp(x[i] - mx); sum += x[i]; }
        float inv = 1.f / sum;
        for (int i = 0; i < n; i++) x[i] *= inv;
    }

    // Sinusoidal embedding: out[i] = cos(val * freqs[i]), out[i+nf] = sin(…).
    inline void sinusoidal_embed(float val, const float* freqs, int nf, float* out) {
        for (int i = 0; i < nf; i++) {
            out[i] = std::cos(val * freqs[i]);
            out[i + nf] = std::sin(val * freqs[i]);
        }
    }

} // namespace math
