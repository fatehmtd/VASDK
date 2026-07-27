#pragma once

// 
// VizAnimImpl - full feature-extraction implementation.
//
// This header is included ONLY from VizAnimProvider.cpp, keeping all heavy
// dependencies (pffft, Google Highway) out of the public API header.
// 

#include "../VizAnimProvider.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <utility>
#include <vector>
#include <memory>
#include <cstdint>

#include <pffft/pffft.h>

// Highway - static dispatch (single target, same pattern as AudioSIMD.cpp).
// We do NOT use the foreach_target / HWY_EXPORT / HWY_DYNAMIC_DISPATCH
// mechanism here because this file is included from exactly one .cpp TU and
// cannot be re-included per-target the way foreach_target.h requires.
#include "hwy/highway.h"
namespace hn = hwy::HWY_NAMESPACE;

// 
// SIMD helpers - static (file-local), compiled for the current target
// 

static float Dot(const float* HWY_RESTRICT a,
                 const float* HWY_RESTRICT b, int n)
{
    const hn::ScalableTag<float> d;
    const int lanes = static_cast<int>(hn::Lanes(d));
    auto acc = hn::Zero(d);
    int i = 0;
    for (; i + lanes <= n; i += lanes)
        acc = hn::MulAdd(hn::Load(d, a + i), hn::Load(d, b + i), acc);
    float result = hn::ReduceSum(d, acc);
    for (; i < n; ++i) result += a[i] * b[i];
    return result;
}

static float SumSq(const float* HWY_RESTRICT x, int n)
{
    const hn::ScalableTag<float> d;
    const int lanes = static_cast<int>(hn::Lanes(d));
    auto acc = hn::Zero(d);
    int i = 0;
    for (; i + lanes <= n; i += lanes) {
        auto v = hn::Load(d, x + i);
        acc = hn::MulAdd(v, v, acc);
    }
    float result = hn::ReduceSum(d, acc);
    for (; i < n; ++i) result += x[i] * x[i];
    return result;
}

static float SqDiffSum(const float* HWY_RESTRICT a,
                       const float* HWY_RESTRICT b, int n)
{
    const hn::ScalableTag<float> d;
    const int lanes = static_cast<int>(hn::Lanes(d));
    auto acc = hn::Zero(d);
    int i = 0;
    for (; i + lanes <= n; i += lanes) {
        auto diff = hn::Sub(hn::Load(d, a + i), hn::Load(d, b + i));
        acc = hn::MulAdd(diff, diff, acc);
    }
    float result = hn::ReduceSum(d, acc);
    for (; i < n; ++i) { float diff = a[i] - b[i]; result += diff * diff; }
    return result;
}

static void Mac(float* HWY_RESTRICT out,
                const float* HWY_RESTRICT in,
                float scale, int n)
{
    const hn::ScalableTag<float> d;
    const int lanes = static_cast<int>(hn::Lanes(d));
    auto vs = hn::Set(d, scale);
    int i = 0;
    for (; i + lanes <= n; i += lanes)
        hn::Store(hn::MulAdd(hn::Load(d, in + i), vs, hn::Load(d, out + i)),
                  d, out + i);
    for (; i < n; ++i) out[i] += scale * in[i];
}

// 
// Constants
// 

static constexpr float kPi = 3.14159265358979f;

// 
// Scalar utilities
// 

static std::vector<float> hann_window(int n)
{
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (n - 1)));
    return w;
}

static std::vector<float> bell_kernel(int half = 10, float sigma = 4.0f)
{
    std::vector<float> k(2 * half + 1);
    float sum = 0.0f;
    for (int i = 0; i <= 2 * half; ++i) {
        float x = static_cast<float>(i - half);
        k[i] = std::exp(-x * x / (2.0f * sigma * sigma));
        sum += k[i];
    }
    for (auto& v : k) v /= sum;
    return k;
}

// Convolution - aligned interior segments use SIMD Dot, edges fall back to scalar.
static std::vector<float> convolve(const std::vector<float>& sig,
                                    const std::vector<float>& k)
{
    const int n    = static_cast<int>(sig.size());
    const int kn   = static_cast<int>(k.size());
    const int half = kn / 2;
    std::vector<float> out(n, 0.0f);

    for (int i = 0; i < n; ++i) {
        if (i >= half && i + half < n) {
            out[i] = Dot(sig.data() + i - half, k.data(), kn);
        } else {
            for (int j = 0; j < kn; ++j)
                out[i] += sig[std::clamp(i + j - half, 0, n - 1)] * k[j];
        }
    }
    return out;
}

// 
// PFFFT RAII wrappers (mirrors the pattern used in SmartTurnV3)
// 

struct PFFTSetupDeleter {
    void operator()(PFFFT_Setup* s) const { if (s) pffft_destroy_setup(s); }
};
struct PFFTBufferDeleter {
    void operator()(float* p) const { if (p) pffft_aligned_free(p); }
};

// 
// FFT context - created once, reused every frame
// 

struct FFTContext {
    int           n;
    int           sampleRate;

    std::unique_ptr<PFFFT_Setup, PFFTSetupDeleter> setup;
    std::unique_ptr<float,       PFFTBufferDeleter> fftInput;
    std::unique_ptr<float,       PFFTBufferDeleter> fftOutput;
    std::unique_ptr<float,       PFFTBufferDeleter> fftWork;

    std::vector<float> window;
    std::vector<float> kernel;
    std::vector<int>   chromaMap;  // bin → pitch class 0-11, or -1

    FFTContext(int fftSize, int sr)
        : n(fftSize)
        , sampleRate(sr)
        , setup(pffft_new_setup(fftSize, PFFFT_REAL))
        , fftInput(static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float))))
        , fftOutput(static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float))))
        , fftWork(static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float))))
        , window(hann_window(fftSize))
        , kernel(bell_kernel(10, 4.0f))
    {
        buildChromaMap();
    }

private:
    void buildChromaMap()
    {
        const int half  = n / 2 + 1;
        const float binHz = static_cast<float>(sampleRate) / n;
        chromaMap.assign(half, -1);
        for (int i = 1; i < half; ++i) {
            float freq = i * binHz;
            if (freq < 32.7f || freq > 4186.0f) continue;
            float midi = 12.0f * std::log2(freq / 440.0f) + 69.0f;
            chromaMap[i] = ((static_cast<int>(std::round(midi)) % 12) + 12) % 12;
        }
    }
};

// 
// Helper: magnitude spectrum from pffft ordered real output
// 

static std::vector<float> magnitude(const float* out, int n)
{
    const int half = n / 2 + 1;
    std::vector<float> mag(half);
    mag[0]        = std::abs(out[0]) / n;
    mag[half - 1] = std::abs(out[1]) / n;
    for (int i = 1; i < half - 1; ++i) {
        float re = out[2 * i], im = out[2 * i + 1];
        mag[i] = std::sqrt(re * re + im * im) / n;
    }
    return mag;
}

// 
// Band energy - average magnitude over a Hz range
// 

static float band_energy(const std::vector<float>& mag,
                          float lo, float hi,
                          int fftSize, int sampleRate)
{
    const float binHz = static_cast<float>(sampleRate) / fftSize;
    const int   a     = static_cast<int>(lo / binHz);
    const int   b     = std::min(static_cast<int>(hi / binHz),
                                  static_cast<int>(mag.size()) - 1);
    if (a > b) return 0.0f;
    float sum = 0.0f;
    for (int i = a; i <= b; ++i) sum += mag[i];
    return sum / (b - a + 1);
}

// 
// Zero-crossing rate
// 

static float zero_crossing_rate(const float* samples, int n)
{
    int count = 0;
    for (int i = 1; i < n; ++i)
        if ((samples[i] >= 0.0f) != (samples[i - 1] >= 0.0f)) ++count;
    return static_cast<float>(count) / (n - 1);
}

// 
// Chroma - scatter magnitude by pitch class, normalise
// 

static std::array<float, va::thirdparty::VizAnimProvider::NUM_CHROMA>
compute_chroma(const std::vector<float>& magSmoothed,
               const FFTContext& ctx)
{
    std::array<float, va::thirdparty::VizAnimProvider::NUM_CHROMA> chroma{};
    float total = 0.0f;
    for (int i = 0; i < static_cast<int>(magSmoothed.size()); ++i) {
        const int pc = ctx.chromaMap[i];
        if (pc >= 0) { chroma[pc] += magSmoothed[i]; total += magSmoothed[i]; }
    }
    if (total > 0.0f)
        for (auto& v : chroma) v /= total;
    return chroma;
}

// 
// Pitch - YIN algorithm with parabolic interpolation
// 

static std::pair<float, float> yin_pitch(const float* samples, int n,
                                          int sampleRate)
{
    const int tauMin = sampleRate / 500;
    const int tauMax = std::min(sampleRate / 50, n / 2);

    // Difference function - hot path uses SIMD SqDiffSum
    std::vector<float> d(tauMax + 1, 0.0f);
    for (int tau = 1; tau <= tauMax; ++tau)
        d[tau] = SqDiffSum(samples, samples + tau, n - tau);

    // Cumulative mean normalised difference
    std::vector<float> dp(tauMax + 1);
    dp[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau <= tauMax; ++tau) {
        running += d[tau];
        dp[tau] = (running > 0.0f) ? d[tau] * tau / running : 1.0f;
    }

    // First dip below threshold, then local minimum
    constexpr float kThreshold = 0.1f;
    int bestTau = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau) {
        if (dp[tau] < kThreshold) {
            while (tau + 1 <= tauMax && dp[tau + 1] < dp[tau]) ++tau;
            bestTau = tau;
            break;
        }
    }

    if (bestTau < 1 || bestTau >= tauMax) return { 0.0f, 0.0f };

    // Parabolic interpolation
    const float s0 = dp[bestTau - 1], s1 = dp[bestTau], s2 = dp[bestTau + 1];
    const float denom = 2.0f * s1 - s0 - s2;
    const float tauF  = (std::abs(denom) > 1e-6f)
                      ? bestTau + 0.5f * (s0 - s2) / denom
                      : static_cast<float>(bestTau);

    return { static_cast<float>(sampleRate) / tauF,
             std::clamp(1.0f - dp[bestTau], 0.0f, 1.0f) };
}

// 
// Main extraction - returns a fully populated Features struct
// 

static va::thirdparty::VizAnimProvider::Features
extract_features(FFTContext& ctx, const float* samples,
                 std::vector<float>& prevMag, float& fluxAvg)
{
    const int n      = ctx.n;
    const int sr     = ctx.sampleRate;
    const int half   = n / 2 + 1;
    const float binHz = static_cast<float>(sr) / n;

    // 1. Apply window and forward FFT
    for (int i = 0; i < n; ++i) {
        ctx.fftInput.get()[i] = samples[i] * ctx.window[i];
    }
    pffft_transform_ordered(ctx.setup.get(),
                            ctx.fftInput.get(),
                            ctx.fftOutput.get(),
                            ctx.fftWork.get(),
                            PFFFT_FORWARD);

    // 2. Magnitude spectrum + bell-smoothed copy
    auto mag   = magnitude(ctx.fftOutput.get(), n);
    auto magS  = convolve(mag, ctx.kernel);

    // 3. RMS (SIMD sum-of-squares)
    const float rms = std::sqrt(SumSq(samples, n) / n);

    // 4. Band energies
    const float bass   = band_energy(magS,    20.0f,   250.0f, n, sr);
    const float lowMid = band_energy(magS,   250.0f,   500.0f, n, sr);
    const float mid    = band_energy(magS,   500.0f,  2000.0f, n, sr);
    const float hiMid  = band_energy(magS,  2000.0f,  4000.0f, n, sr);
    const float treble = band_energy(magS,  4000.0f, 20000.0f, n, sr);

    // 5. Spectral centroid (normalised to [0,1])
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < half; ++i) {
        num += i * binHz * magS[i];
        den += magS[i];
    }
    const float centroid = (den > 0.0f) ? (num / den) / (sr / 2.0f) : 0.0f;

    // 6. Spectral rolloff - frequency below which 85% of energy accumulates
    float rolloff = 0.0f;
    {
        float acc = 0.0f;
        for (int i = 0; i < half; ++i) {
            acc += magS[i];
            if (acc >= 0.85f * den) {
                rolloff = (i * binHz) / (sr / 2.0f);
                break;
            }
        }
    }

    // 7. Spectral flux + onset (SIMD squared-diff sum)
    float flux = 0.0f;
    if (!prevMag.empty())
        flux = SqDiffSum(magS.data(), prevMag.data(), half);
    prevMag  = magS;
    fluxAvg  = fluxAvg * 0.9f + flux * 0.1f;
    const bool onset = (flux > fluxAvg * 1.8f) && (rms > 0.01f);

    // 8. Advanced features
    const float zcr                   = zero_crossing_rate(samples, n);
    auto [pitch, voiced]              = yin_pitch(samples, n, sr);
    auto chroma                       = compute_chroma(magS, ctx);

    return {
        rms, bass, lowMid, mid, hiMid, treble,
        centroid, rolloff, flux, onset,
        zcr, pitch, voiced, chroma
    };
}

// 
// VizAnimProvider::VizAnimImpl - owns all extraction state
// 

namespace va {
namespace thirdparty {

class VizAnimProvider::VizAnimImpl {
public:
    //static constexpr int kDefaultFftSize = 2048;

    explicit VizAnimImpl(int sampleRate, int fftSize)
        : _ctx(fftSize, sampleRate)
        , _fluxAvg(0.0f)
    {}

    // Non-copyable
    VizAnimImpl(const VizAnimImpl&)            = delete;
    VizAnimImpl& operator=(const VizAnimImpl&) = delete;

    ~VizAnimImpl() = default;

    int fftSize() const { return _ctx.n; }

    /**
     * @brief Extract features from exactly fftSize() float samples.
     *        Caller is responsible for passing the correct number of samples.
     */
    VizAnimProvider::Features extract(const float* samples)
    {
        return extract_features(_ctx, samples, _prevMag, _fluxAvg);
    }

private:
    FFTContext         _ctx;
    std::vector<float> _prevMag;
    float              _fluxAvg;
};

} // namespace thirdparty
} // namespace va