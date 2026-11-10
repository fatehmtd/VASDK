#include <VASDK/WavFile.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace va {
namespace audio {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t little16(const unsigned char* bytes) { return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)); }
uint32_t little32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
void putLittle16(std::string& out, uint16_t value) {
    out += static_cast<char>(value & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
}
void putLittle32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((value >> shift) & 0xFF);
}

bool fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

float clipped(float sample) { return std::isnan(sample) ? 0.0f : std::min(1.0f, std::max(-1.0f, sample)); }

} // namespace

bool readWav(const std::string& path, WavAudio& audio, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(error, path + ": can't be opened");
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return fail(error, path + ": not a RIFF/WAVE file");

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* samples = nullptr;
    size_t sampleBytes = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = little32(data + at + 4);
        const size_t body = at + 8;
        const size_t available = std::min<size_t>(size, bytes.size() - body);   // a cut file: what is there
        if (std::memcmp(data + at, "fmt ", 4) == 0 && available >= 16) {
            format = little16(data + body);
            channels = little16(data + body + 2);
            rate = little32(data + body + 4);
            bits = little16(data + body + 14);
            if (format == kFormatExtensible && available >= 26) format = little16(data + body + 24);   // the subformat's first 2 bytes
        } else if (std::memcmp(data + at, "data", 4) == 0) {
            samples = data + body;
            sampleBytes = available;
        }
        at = body + size + (size & 1);   // chunks are padded to even sizes
    }
    if (!channels || !rate) return fail(error, path + ": no fmt chunk");
    if (!samples) return fail(error, path + ": no data chunk");
    if (!(format == kFormatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(format == kFormatFloat && bits == 32))
        return fail(error, path + ": format " + std::to_string(format) + " with " + std::to_string(bits) + " bits isn't read");

    const size_t width = bits / 8;
    const size_t count = sampleBytes / width;
    audio.sampleRate = static_cast<int>(rate);
    audio.channels = channels;
    audio.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* sample = samples + i * width;
        float value = 0.0f;
        if (format == kFormatFloat) {
            const uint32_t raw = little32(sample);
            std::memcpy(&value, &raw, sizeof(value));
        } else if (bits == 8) {
            value = (static_cast<int>(sample[0]) - 128) / 128.0f;   // 8-bit PCM is unsigned
        } else if (bits == 16) {
            value = static_cast<int16_t>(little16(sample)) / 32768.0f;
        } else if (bits == 24) {
            const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(sample[0]) << 8) | (static_cast<uint32_t>(sample[1]) << 16) |
                                                     (static_cast<uint32_t>(sample[2]) << 24)) >> 8;
            value = raw / 8388608.0f;
        } else {
            value = static_cast<float>(static_cast<int32_t>(little32(sample)) / 2147483648.0);
        }
        audio.samples[i] = value;
    }
    return true;
}

bool writeWav(const std::string& path, const float* samples, size_t count, int sampleRate, int channels, AudioFormat format,
              std::string* error) {
    if (sampleRate <= 0 || channels <= 0) return fail(error, path + ": no sample rate or channels");
    if (format != AudioFormat::SHORT && format != AudioFormat::INT && format != AudioFormat::FLOAT)
        return fail(error, path + ": WAV files are written as SHORT, INT or FLOAT");
    const uint16_t bits = format == AudioFormat::SHORT ? 16 : 32;
    const uint32_t dataBytes = static_cast<uint32_t>(count * (bits / 8));
    std::string out;
    out.reserve(44 + dataBytes);
    out += "RIFF";
    putLittle32(out, 36 + dataBytes);
    out += "WAVEfmt ";
    putLittle32(out, 16);
    putLittle16(out, format == AudioFormat::FLOAT ? kFormatFloat : kFormatPcm);
    putLittle16(out, static_cast<uint16_t>(channels));
    putLittle32(out, static_cast<uint32_t>(sampleRate));
    putLittle32(out, static_cast<uint32_t>(sampleRate * channels * (bits / 8)));
    putLittle16(out, static_cast<uint16_t>(channels * (bits / 8)));
    putLittle16(out, bits);
    out += "data";
    putLittle32(out, dataBytes);
    for (size_t i = 0; i < count; ++i) {
        if (format == AudioFormat::SHORT) {
            putLittle16(out, static_cast<uint16_t>(static_cast<int16_t>(std::lround(clipped(samples[i]) * 32767.0f))));
        } else if (format == AudioFormat::INT) {
            putLittle32(out, static_cast<uint32_t>(static_cast<int32_t>(std::llround(clipped(samples[i]) * 2147483647.0))));
        } else {
            uint32_t raw = 0;
            std::memcpy(&raw, &samples[i], sizeof(raw));
            putLittle32(out, raw);
        }
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) return fail(error, path + ": can't be written");
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file.good() || fail(error, path + ": write failed");
}

std::vector<short> toPcm16(const float* samples, size_t count) {
    std::vector<short> out(count);
    for (size_t i = 0; i < count; ++i) out[i] = static_cast<short>(std::lround(clipped(samples[i]) * 32767.0f));
    return out;
}

std::vector<float> fromPcm16(const short* samples, size_t count) {
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i) out[i] = samples[i] / 32768.0f;
    return out;
}

} // namespace audio
} // namespace va
