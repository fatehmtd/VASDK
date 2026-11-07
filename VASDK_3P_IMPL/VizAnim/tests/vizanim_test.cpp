// VizAnimProvider on synthetic audio: a 440 Hz tone gives its level and pitch, silence gives none,
// and processAudio passes the audio through unchanged.
#include "VizAnim/VizAnimProvider.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

} // namespace

int main() {
    va::audio::AudioConfig config;
    config.sampleRate = 16000;
    config.channels = 1;
    config.format = va::audio::AudioFormat::FLOAT;

    va::thirdparty::audio::VizAnimProvider provider;
    expect(provider.initialize(config), "initializes at 16 kHz float");

    const int length = 2 * va::thirdparty::audio::VizAnimProvider::FFT_SIZE;
    std::vector<float> tone(length), silence(length, 0.0f);
    for (int i = 0; i < length; ++i) tone[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / 16000.0f);

    const auto toneFeatures = provider.computeFeatures(reinterpret_cast<const char*>(tone.data()), length);
    expect(!toneFeatures.empty(), "computes features for two analysis windows of a tone");
    if (!toneFeatures.empty()) {
        const auto& features = toneFeatures.back();
        std::printf("     rms %.3f, pitch %.1f Hz, voiced %.2f\n", features.rms, features.pitch, features.voiced);
        expect(features.rms > 0.1f, "measures the tone's level");
        expect(std::fabs(features.pitch - 440.0f) < 20.0f, "finds the tone's pitch near 440 Hz");
    }

    const auto silentFeatures = provider.computeFeatures(reinterpret_cast<const char*>(silence.data()), length);
    expect(!silentFeatures.empty() && silentFeatures.back().rms < 1e-3f, "measures silence as silent");

    std::vector<float> passed(length, 1.0f);
    const size_t written = provider.processAudio(reinterpret_cast<const char*>(tone.data()),
                                                 reinterpret_cast<char*>(passed.data()), length);
    expect(written == static_cast<size_t>(length) && passed == tone, "passes the audio through unchanged");

    va::thirdparty::audio::VizAnimOptions small;
    small.fftSize = 1024;
    va::thirdparty::audio::VizAnimProvider smallWindow(small);
    expect(smallWindow.initialize(config), "initializes with a 1024-point FFT");
    return failures == 0 ? 0 : 1;
}
