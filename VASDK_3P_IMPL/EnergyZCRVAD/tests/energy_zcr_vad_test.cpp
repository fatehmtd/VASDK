// EnergyZCRVAD on synthetic audio: silence and hiss are not speech, a loud low tone is; options change the verdict.
#include "EnergyZCRVAD/EnergyZCRVAD.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

va::audio::AudioConfig floatAudio() {
    va::audio::AudioConfig config;
    config.sampleRate = 16000;
    config.channels = 1;
    config.format = va::audio::AudioFormat::FLOAT;
    return config;
}

bool voiced(const va::thirdparty::vad::EnergyZCRVAD& vad, const std::vector<float>& samples) {
    return !vad.detectVoiceActivity(reinterpret_cast<const char*>(samples.data()), 0, samples.size()).empty();
}

} // namespace

int main() {
    const size_t length = 16000 / 10;  // 100 ms
    std::vector<float> silence(length, 0.0f);
    std::vector<float> tone(length);    // 200 Hz at -12 dBFS: loud, few zero crossings, like a voiced vowel
    for (size_t i = 0; i < length; ++i) tone[i] = 0.25f * std::sin(2.0f * 3.14159265f * 200.0f * i / 16000.0f);
    std::vector<float> hiss(length);    // loud white noise: crosses zero about every other sample
    std::mt19937 generator(7);
    std::uniform_real_distribution<float> noise(-0.25f, 0.25f);
    for (float& sample : hiss) sample = noise(generator);

    va::thirdparty::vad::EnergyZCRVAD vad;
    expect(vad.initialize(floatAudio(), static_cast<int>(length)), "initializes with the default options");
    expect(!voiced(vad, silence), "silence is not speech");
    expect(voiced(vad, tone), "a loud low tone is speech");
    expect(!voiced(vad, hiss), "loud hiss is not speech (zero-crossing rate too high)");

    va::thirdparty::vad::EnergyZCRVADOptions deaf;
    deaf.energyThreshold = 1e6;
    va::thirdparty::vad::EnergyZCRVAD deafVad(deaf);
    deafVad.initialize(floatAudio(), static_cast<int>(length));
    expect(!voiced(deafVad, tone), "a higher energy threshold rejects the tone");

    va::thirdparty::vad::EnergyZCRVADOptions permissive;
    permissive.zeroCrossingRateThreshold = 1.0;
    va::thirdparty::vad::EnergyZCRVAD permissiveVad(permissive);
    permissiveVad.initialize(floatAudio(), static_cast<int>(length));
    expect(voiced(permissiveVad, hiss), "a zero-crossing threshold of 1 lets hiss through");

    return failures == 0 ? 0 : 1;
}
