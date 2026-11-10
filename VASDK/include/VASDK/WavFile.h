#pragma once

// WAV files in and out: RIFF/WAVE with PCM samples of 8, 16, 24 or 32 bits, or 32-bit float
// (WAVE_FORMAT_EXTENSIBLE too). Samples cross as float in [-1, 1], interleaved by channel, with
// the file's rate and channel count beside them; converting rate or channels is the caller's.
//
//   va::audio::WavAudio audio;
//   std::string error;
//   if (!va::audio::readWav("clip.wav", audio, &error)) std::cerr << error << '\n';
//   va::audio::writeWav("copy.wav", audio.samples.data(), audio.samples.size(), audio.sampleRate, audio.channels);
//
// Tests, evaluation and tools use it (VASDK_3P_IMPL/Evaluation); the pipeline reads audio
// through its transports.

#include <VASDK/CommonAudioUtils.h>
#include <VASDK/vasdk_export.h>

#include <cstddef>
#include <string>
#include <vector>

namespace va {
namespace audio {

struct WavAudio {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;   // interleaved, in [-1, 1]

    /// Frames: samples per channel
    size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
    /// Length in seconds
    double seconds() const { return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
};

/// Reads a WAV file. False, with `error` set, when it can't be opened or isn't a WAV file it reads.
VASDK_EXPORT bool readWav(const std::string& path, WavAudio& audio, std::string* error = nullptr);

/// Writes `count` interleaved samples as a WAV file in `format`: SHORT (16-bit PCM, the default),
/// INT (32-bit PCM) or FLOAT (32-bit float). Samples beyond [-1, 1] are clipped for PCM.
VASDK_EXPORT bool writeWav(const std::string& path, const float* samples, size_t count, int sampleRate, int channels,
                           AudioFormat format = AudioFormat::SHORT, std::string* error = nullptr);

/// Float samples in [-1, 1] as 16-bit PCM, clipped
VASDK_EXPORT std::vector<short> toPcm16(const float* samples, size_t count);
/// 16-bit PCM as float samples in [-1, 1]
VASDK_EXPORT std::vector<float> fromPcm16(const short* samples, size_t count);

} // namespace audio
} // namespace va
