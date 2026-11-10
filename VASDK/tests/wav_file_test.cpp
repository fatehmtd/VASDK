// WAV files: what writeWav writes, readWav reads back, in each format it writes; 8- and 24-bit PCM
// and WAVE_FORMAT_EXTENSIBLE files built by hand; a chunk before "fmt " and an odd-sized one are
// skipped; a file that isn't WAV fails with a reason.

#include <VASDK/WavFile.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace va::audio;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

std::string little(uint32_t value, int bytes) {
    std::string out;
    for (int i = 0; i < bytes; ++i) out += static_cast<char>((value >> (8 * i)) & 0xFF);
    return out;
}

// A WAV file by hand: an odd-sized "LIST" chunk first, then fmt (extensible when asked), then data.
std::string handMade(uint16_t format, uint16_t bits, uint16_t channels, const std::string& data, bool extensible) {
    std::string fmt = little(extensible ? 0xFFFE : format, 2) + little(channels, 2) + little(8000, 4) +
                      little(8000 * channels * bits / 8, 4) + little(channels * bits / 8, 2) + little(bits, 2);
    if (extensible) fmt += little(22, 2) + little(bits, 2) + little(0, 4) + little(format, 2) + std::string(14, '\0');
    std::string body = "WAVE";
    body += "LIST" + little(3, 4) + "abc" + std::string(1, '\0');   // padded to even
    body += "fmt " + little(static_cast<uint32_t>(fmt.size()), 4) + fmt;
    body += "data" + little(static_cast<uint32_t>(data.size()), 4) + data;
    return "RIFF" + little(static_cast<uint32_t>(body.size()), 4) + body;
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

int main() {
    const auto folder = std::filesystem::temp_directory_path() / "vasdk_wav_file_test";
    std::filesystem::create_directories(folder);

    // Round trips: a 440 Hz stereo tone, past full scale at one sample (clipped for PCM)
    std::vector<float> tone(2 * 1600);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.8f * std::sin(static_cast<float>(i / 2) * 2.0f * 3.14159265f * 440.0f / 16000.0f);
    tone[10] = 1.5f;
    for (const auto format : {AudioFormat::SHORT, AudioFormat::INT, AudioFormat::FLOAT}) {
        const auto path = (folder / ("tone" + std::to_string(format) + ".wav")).string();
        std::string error;
        check(writeWav(path, tone.data(), tone.size(), 16000, 2, format, &error), "write " + std::to_string(format) + ": " + error);
        WavAudio audio;
        check(readWav(path, audio, &error), "read " + std::to_string(format) + ": " + error);
        check(audio.sampleRate == 16000 && audio.channels == 2, "rate and channels kept");
        check(audio.samples.size() == tone.size() && audio.frames() == 1600, "every sample back");
        check(near(static_cast<float>(audio.seconds()), 0.1f, 1e-6f), "0.1 s");
        const float tolerance = format == AudioFormat::SHORT ? 1.0f / 16000 : 1e-6f;
        bool same = audio.samples.size() == tone.size();
        for (size_t i = 0; same && i < tone.size(); ++i)
            if (i != 10) same = near(audio.samples[i], tone[i], tolerance);
        check(same, "samples kept, format " + std::to_string(format));
        if (audio.samples.size() > 10)
            check(format == AudioFormat::FLOAT ? audio.samples[10] == 1.5f : near(audio.samples[10], 1.0f, 1e-4f),
                  "past full scale: clipped in PCM, kept in float");
    }

    // 8-bit (unsigned) and 24-bit PCM, plain and extensible, mono
    {
        const auto path = (folder / "eight.wav").string();
        writeFile(path, handMade(1, 8, 1, std::string{'\x80', '\xC0', '\x40', '\xFF'}, false));
        WavAudio audio;
        std::string error;
        check(readWav(path, audio, &error), "8-bit: " + error);
        check(audio.sampleRate == 8000 && audio.samples.size() == 4, "8-bit: 4 samples at 8 kHz");
        if (audio.samples.size() == 4)
            check(audio.samples[0] == 0.0f && audio.samples[1] == 0.5f && audio.samples[2] == -0.5f && near(audio.samples[3], 127 / 128.0f, 1e-6f),
                  "8-bit values");
    }
    for (const bool extensible : {false, true}) {
        const auto path = (folder / (extensible ? "24x.wav" : "24.wav")).string();
        // 0x400000 = 0.5, 0xC00000 = -0.5
        writeFile(path, handMade(1, 24, 1, std::string{'\0', '\0', '\x40', '\0', '\0', '\xC0'}, extensible));
        WavAudio audio;
        std::string error;
        check(readWav(path, audio, &error), "24-bit: " + error);
        check(audio.samples.size() == 2 && near(audio.samples[0], 0.5f, 1e-6f) && near(audio.samples[1], -0.5f, 1e-6f),
              extensible ? "24-bit extensible values" : "24-bit values");
    }

    // Not WAV, missing, and a format it doesn't read
    {
        const auto path = (folder / "text.wav").string();
        writeFile(path, "not a wav file at all");
        WavAudio audio;
        std::string error;
        check(!readWav(path, audio, &error) && error.find("RIFF") != std::string::npos, "text: refused, says why");
        check(!readWav((folder / "missing.wav").string(), audio, &error), "missing file refused");
        const auto alaw = (folder / "alaw.wav").string();
        writeFile(alaw, handMade(6, 8, 1, "ab", false));
        check(!readWav(alaw, audio, &error) && error.find("format 6") != std::string::npos, "A-law refused, says which format");
    }

    // PCM16 helpers
    {
        const float samples[] = {0.0f, 1.0f, -1.0f, 2.0f, 0.5f};
        const auto pcm = toPcm16(samples, 5);
        check(pcm[0] == 0 && pcm[1] == 32767 && pcm[2] == -32767 && pcm[3] == 32767 && pcm[4] == 16384, "toPcm16 scales and clips");
        const auto back = fromPcm16(pcm.data(), pcm.size());
        check(near(back[4], 0.5f, 1e-4f), "fromPcm16");
    }

    std::filesystem::remove_all(folder);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("wav_file: all passed\n");
    return 0;
}
