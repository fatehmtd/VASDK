// RNNoiseDSPProvider with its built-in model on synthetic audio: steady hiss comes out much quieter,
// the sample count is kept, and stereo input is refused.
#include "RNNoise/RNNoiseDSPProvider.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

va::audio::AudioConfig monoShort(int channels = 1) {
    va::audio::AudioConfig config;
    config.sampleRate = 16000;
    config.channels = channels;
    config.format = va::audio::AudioFormat::SHORT;
    return config;
}

double energy(const std::vector<int16_t>& samples, size_t from, size_t to) {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += double(samples[i]) * samples[i];
    return sum / double(to - from);
}

} // namespace

int main() {
    va::thirdparty::RNNoiseDSPProvider denoiser;
    expect(denoiser.initialize(monoShort()), "initializes at 16 kHz mono with the built-in model");
    expect(denoiser.getName() == "RNNoise", "is named RNNoise");

    // 3 s of white noise at about -20 dBFS, in 20 ms blocks as a pipeline would send it.
    const size_t block = 320, blocks = 150;
    std::mt19937 generator(3);
    std::normal_distribution<double> noise(0.0, 3200.0);
    std::vector<int16_t> input(block * blocks), output(block * blocks, 0);
    for (int16_t& sample : input) sample = static_cast<int16_t>(std::lround(noise(generator)));

    size_t produced = 0;
    for (size_t b = 0; b < blocks; ++b) {
        produced += denoiser.processAudio(reinterpret_cast<const char*>(input.data() + b * block),
                                          reinterpret_cast<char*>(output.data() + b * block), block);
    }
    expect(produced == input.size(), "returns as many samples as it was given");

    // Past the first second the model has adapted: hiss should drop by well over 10 dB.
    const double before = energy(input, input.size() / 3, input.size());
    const double after  = energy(output, output.size() / 3, output.size());
    std::printf("     noise energy in %.0f, out %.0f (%.1f dB)\n", before, after, 10.0 * std::log10(after / before));
    expect(after < before / 10.0, "suppresses steady hiss by more than 10 dB");
    expect(denoiser.getLastVADProbability() < 0.5f, "does not take hiss for voice");
    denoiser.release();

    va::thirdparty::RNNoiseDSPProvider stereo;
    expect(!stereo.initialize(monoShort(2)), "refuses stereo input");

    va::thirdparty::RNNoiseDSPOptions missingModel;
    missingModel.modelPath = "/no/such/model.rnnn";
    va::thirdparty::RNNoiseDSPProvider custom(missingModel);
    expect(!custom.initialize(monoShort()), "fails to initialize with a model file that doesn't exist");

    return failures == 0 ? 0 : 1;
}
