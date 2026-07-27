#include "AudioSIMD.h"

#include <algorithm>
#include <cstring>

#ifdef VASDK_HIGHWAY_ENABLED
#include "hwy/highway.h"
namespace hn = hwy::HWY_NAMESPACE;
#endif

namespace AudioSIMD {

// 
// convertInt16ToFloat
//   Scalar baseline : out[i] = in[i] / 32768.f
//   SIMD path       : PromoteTo int16→int32, ConvertTo int32→float, Mul
// 

void convertInt16ToFloat(const int16_t* in, float* out, size_t n)
{
#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float>             df;
    const hn::Half<hn::ScalableTag<int16_t>> dhi16; // same lane count as df
    const hn::ScalableTag<int32_t>           di32;
    const auto kScale = hn::Set(df, 1.0f / 32768.0f);
    const size_t step = hn::Lanes(dhi16);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        const auto vi32 = hn::PromoteTo(di32, hn::LoadU(dhi16, in + i));
        hn::StoreU(hn::Mul(hn::ConvertTo(df, vi32), kScale), df, out + i);
    }
    for (; i < n; ++i)
        out[i] = static_cast<float>(in[i]) * (1.0f / 32768.0f);
#else
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<float>(in[i]) * (1.0f / 32768.0f);
#endif
}

// 
// convertInt32ToFloat
//   Scalar baseline : out[i] = in[i] / 2^31
//   SIMD path       : ConvertTo int32→float, Mul
// 

void convertInt32ToFloat(const int32_t* in, float* out, size_t n)
{
#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float>   df;
    const hn::ScalableTag<int32_t> di32;
    const auto kScale = hn::Set(df, 1.0f / 2147483648.0f);
    const size_t step = hn::Lanes(df);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        hn::StoreU(
            hn::Mul(hn::ConvertTo(df, hn::LoadU(di32, in + i)), kScale),
            df, out + i);
    }
    for (; i < n; ++i)
        out[i] = static_cast<float>(in[i]) * (1.0f / 2147483648.0f);
#else
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<float>(in[i]) * (1.0f / 2147483648.0f);
#endif
}

// 
// convertByteToFloat  (scalar - triple-widening SIMD adds complexity for a
//                      rarely-used format; leave for a follow-up)
// 

void convertByteToFloat(const uint8_t* in, float* out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = (static_cast<float>(in[i]) - 128.0f) * (1.0f / 128.0f);
}

// 
// convertFloatToInt16
//   Scalar baseline : clamp → * 32767 → truncate
//   SIMD path       : Clamp, Mul, ConvertTo float→int32, DemoteTo int32→int16
// 

void convertFloatToInt16(const float* in, int16_t* out, size_t n)
{
#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float>             df;
    const hn::ScalableTag<int32_t>           di32;
    const hn::Half<hn::ScalableTag<int16_t>> dhi16;
    const auto kMin   = hn::Set(df, -1.0f);
    const auto kMax   = hn::Set(df,  1.0f);
    const auto kScale = hn::Set(df, 32767.0f);
    const size_t step = hn::Lanes(df);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        const auto vf   = hn::Mul(hn::Clamp(hn::LoadU(df, in + i), kMin, kMax), kScale);
        const auto vi16 = hn::DemoteTo(dhi16, hn::ConvertTo(di32, vf));
        hn::StoreU(vi16, dhi16, out + i);
    }
    for (; i < n; ++i)
        out[i] = static_cast<int16_t>(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
#else
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<int16_t>(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
#endif
}

// 
// convertFloatToInt32
//   Scalar baseline : clamp → * (2^31 − 1) → truncate
//   SIMD path       : Clamp, Mul, ConvertTo float→int32
// 

void convertFloatToInt32(const float* in, int32_t* out, size_t n)
{
#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float>   df;
    const hn::ScalableTag<int32_t> di32;
    const auto kMin   = hn::Set(df, -1.0f);
    const auto kMax   = hn::Set(df,  1.0f);
    const auto kScale = hn::Set(df, 2147483647.0f);
    const size_t step = hn::Lanes(df);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        hn::StoreU(
            hn::ConvertTo(di32,
                hn::Mul(hn::Clamp(hn::LoadU(df, in + i), kMin, kMax), kScale)),
            di32, out + i);
    }
    for (; i < n; ++i)
        out[i] = static_cast<int32_t>(std::clamp(in[i], -1.0f, 1.0f) * 2147483647.0f);
#else
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<int32_t>(std::clamp(in[i], -1.0f, 1.0f) * 2147483647.0f);
#endif
}

// 
// convertFloatToUint8  (scalar - triple-narrowing SIMD adds complexity for a
//                       rarely-used format; leave for a follow-up)
// 

void convertFloatToUint8(const float* in, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<uint8_t>((std::clamp(in[i], -1.0f, 1.0f) + 1.0f) * 127.5f);
}

// 
// computeEnergy
//   Scalar baseline : Σ data[i]²
//   SIMD path       : MulAdd accumulation, ReduceSum
// 

float computeEnergy(const float* data, size_t n)
{
#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float> df;
    const size_t step = hn::Lanes(df);
    auto vSum = hn::Zero(df);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        const auto v = hn::LoadU(df, data + i);
        vSum = hn::MulAdd(v, v, vSum);
    }
    float sum = hn::ReduceSum(df, vSum);
    for (; i < n; ++i)
        sum += data[i] * data[i];
    return sum;
#else
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i)
        sum += data[i] * data[i];
    return sum;
#endif
}

// 
// computeZCR
//   Scalar baseline : count sign changes in consecutive pairs
//   SIMD path       : load overlapping pairs, Mul, Lt(result, 0) → CountTrue
// 

int computeZCR(const float* data, size_t n)
{
    if (n <= 1) return 0;

#ifdef VASDK_HIGHWAY_ENABLED
    const hn::ScalableTag<float> df;
    const size_t step = hn::Lanes(df);
    const auto kZero  = hn::Zero(df);
    int crossings = 0;
    size_t i = 0;
    for (; i + step + 1 <= n; i += step) {
        const auto v0  = hn::LoadU(df, data + i);
        const auto v1  = hn::LoadU(df, data + i + 1);
        const auto neg = hn::Lt(hn::Mul(v0, v1), kZero);
        crossings += static_cast<int>(hn::CountTrue(df, neg));
    }
    for (; i < n - 1; ++i) {
        if ((data[i] >= 0.0f && data[i + 1] < 0.0f) ||
            (data[i] <  0.0f && data[i + 1] >= 0.0f))
            ++crossings;
    }
    return crossings;
#else
    int crossings = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        if ((data[i] >= 0.0f && data[i + 1] < 0.0f) ||
            (data[i] <  0.0f && data[i + 1] >= 0.0f))
            ++crossings;
    }
    return crossings;
#endif
}

// 
// linearResampleFloat
//   Linear interpolation: read inLen samples, write outLen samples.
//   Variable-stride gather makes this hard to SIMD; scalar is ~3 ops/sample.
// 

void linearResampleFloat(const float* in, size_t inLen, float* out, size_t outLen)
{
    if (inLen == 0 || outLen == 0) return;
    if (inLen == outLen) {
        std::memcpy(out, in, outLen * sizeof(float));
        return;
    }
    const double ratio = static_cast<double>(inLen) / static_cast<double>(outLen);
    for (size_t j = 0; j < outLen; ++j) {
        const double srcPos = static_cast<double>(j) * ratio;
        const size_t idx    = static_cast<size_t>(srcPos);
        const double frac   = srcPos - static_cast<double>(idx);
        const float  s0 = (idx < inLen)     ? in[idx]     : in[inLen - 1];
        const float  s1 = (idx + 1 < inLen) ? in[idx + 1] : s0;
        out[j] = static_cast<float>(s0 + frac * (s1 - s0));
    }
}

} // namespace AudioSIMD
