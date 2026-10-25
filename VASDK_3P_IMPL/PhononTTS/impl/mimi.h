#pragma once
// mimi.h — Mimi neural codec decoder interface.
// Takes a sequence of latent vectors (output of the Flow-LM generation loop)
// and returns float32 PCM samples at 24 kHz.

#include "model_types.h"
#include <vector>

namespace gradium::tts {

// Carried state for chunked (streaming) Mimi decoding.  Every stage of the
// decoder keeps exactly the left-context it needs across chunk boundaries:
//   - transformer: persistent sliding-window KV cache (positions rebased so
//     the cache stays bounded on long streams),
//   - causal convs: the last (kernel-1)*dilation input rows,
//   - transposed convs / depthwise upsample: the last (kernel-stride) raw
//     output rows (pre-bias), added into the next chunk.
// Decoding a stream chunk-by-chunk with one state produces bit-identical PCM
// to decoding all frames at once.
struct MimiStreamState {
    KVCache kv;        // transformer KV
    int past = 0;      // rows already in the cache
    int pos_base = 0;  // absolute position of cache slot 0 (RoPE)

    // Largest chunk (in latent frames) this stream will ever receive.  The KV
    // cache geometry is fixed on the first call, so set this before streaming
    // when later chunks may be bigger than the first (e.g. a ramp-up schedule).
    // 0 → the first chunk is assumed to be the largest.
    int max_chunk_frames = 0;

    std::vector<float> up_carry;  // 16 rows × dimension   (dw upsample, k=32 s=16)
    std::vector<float> c0_prev;   //  6 rows × dimension   (conv k=7)
    std::vector<float> t2_carry;  //  6 rows × 256         (convtr k=12 s=6)
    std::vector<float> r3_prev;   //  2 rows × 256         (resblock conv k=3)
    std::vector<float> t5_carry;  //  5 rows × 128         (convtr k=10 s=5)
    std::vector<float> r6_prev;   //  2 rows × 128
    std::vector<float> t8_carry;  //  4 rows × 64          (convtr k=8 s=4)
    std::vector<float> r9_prev;   //  2 rows × 64
    std::vector<float> c11_prev;  //  2 rows × 64          (conv k=3)
    bool initialized = false;
};

// Decode the next chunk of latent frames to PCM, carrying decoder state
// across calls.  Returns exactly N × 1920 samples for N input frames.
std::vector<float> mimi_decode_stream(const std::vector<std::vector<float>>& latents,
                                      const Config& cfg,
                                      const ModelWeights& weights,
                                      MimiStreamState& state);

// Decode a batch of latent frames to PCM in one shot (fresh state).
// latents: N × ldim vectors produced by the autoregressive loop.
// Returns N × 1920 float32 samples (16× dw-upsample × 6 × 5 × 4 CNN strides).
std::vector<float> mimi_decode(const std::vector<std::vector<float>>& latents,
                               const Config& cfg,
                               const ModelWeights& weights);

} // namespace gradium::tts
