#pragma once

#include "vasdk_export.h"

#include <cstddef>
#include <cstdint>

// 
// AudioSIMD - SIMD-accelerated audio DSP helpers (Google Highway backend)
//
// All pointer pairs must be non-overlapping.  When built without Highway
// (VASDK_HIGHWAY_ENABLED not defined) every function falls back to a
// portable scalar implementation with identical numerical behaviour.
// 

namespace AudioSIMD {

//  Format conversion 

/// Normalise PCM int16 → float in [-1, 1]   (divides by 32768)
VASDK_EXPORT void convertInt16ToFloat(const int16_t* in, float* out, size_t n);

/// Normalise PCM int32 → float in [-1, 1]   (divides by 2^31)
VASDK_EXPORT void convertInt32ToFloat(const int32_t* in, float* out, size_t n);

/// Normalise PCM uint8 → float in [-1, 1]   ((x − 128) / 128)
VASDK_EXPORT void convertByteToFloat(const uint8_t* in, float* out, size_t n);

/// Normalised float → int16   (clamp to [-1,1], multiply by 32767)
VASDK_EXPORT void convertFloatToInt16(const float* in, int16_t* out, size_t n);

/// Normalised float → int32   (clamp to [-1,1], multiply by 2^31 − 1)
VASDK_EXPORT void convertFloatToInt32(const float* in, int32_t* out, size_t n);

/// Normalised float → uint8   (clamp to [-1,1], (x + 1) * 127.5)
VASDK_EXPORT void convertFloatToUint8(const float* in, uint8_t* out, size_t n);

//  DSP helpers 

/// Sum of squared samples  Σ data[i]²
VASDK_EXPORT float computeEnergy(const float* data, size_t n);

/// Number of sign changes between consecutive samples
VASDK_EXPORT int computeZCR(const float* data, size_t n);

//  Resampling 

/// Linear-interpolation resample: read in[0..inLen) → write out[0..outLen)
VASDK_EXPORT void linearResampleFloat(const float* in, size_t inLen, float* out, size_t outLen);

} // namespace AudioSIMD
