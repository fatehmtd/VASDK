// End-to-end smoke test for SupertonicVoiceSynthesisProvider: synthesizes text through the full
// ONNX pipeline at the pipeline's 16 kHz int16 format and writes a WAV for listening.
//
// Usage: supertonic_tts_synth_test <model_dir> [out.wav] [lang] [text]
// Exits 77 (skipped) when the model files are not present.

#include "SupertonicTTS/SupertonicVoiceSynthesisProvider.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Collector : va::audio::VoiceSynthesisResultListener {
    std::mutex              m;
    std::condition_variable cv;
    std::vector<int16_t>    samples;
    bool done = false, error = false;

    void onSynthesisStarted() override {}
    void onSynthesisCompleted() override { std::lock_guard<std::mutex> l(m); done = true; cv.notify_all(); }
    void onSynthesisError(const std::string& e) override {
        std::fprintf(stderr, "synthesis error: %s\n", e.c_str());
        std::lock_guard<std::mutex> l(m); error = done = true; cv.notify_all();
    }
    void onAudioDataReceived(const char* data, size_t size) override {
        std::lock_guard<std::mutex> l(m);
        const auto* p = reinterpret_cast<const int16_t*>(data);
        samples.insert(samples.end(), p, p + size / sizeof(int16_t));
    }
};

void writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataSize = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(dataSize);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <model_dir> [out.wav] [lang] [text]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path modelDir = argv[1];
    const std::string outPath = argc > 2 ? argv[2] : "supertonic_tts_test.wav";
    // Default to a language Supertonic 2 lacked, so the test exercises the v3 model.
    const std::string lang = argc > 3 ? argv[3] : "de";
    const std::string text = argc > 4 ? argv[4]
        : "Hallo! Dies ist ein kurzer Test der Sprachsynthese, komplett offline.";

    const auto onnxDir   = modelDir / "onnx";
    const auto stylePath = modelDir / "voice_styles" / "M1.json";
    if (!std::filesystem::exists(onnxDir / "tts.json") || !std::filesystem::exists(stylePath)) {
        std::printf("Supertonic model files not found under %s - skipped\n", modelDir.string().c_str());
        return 77;
    }

    constexpr int kRate = 16000;
    va::audio::AudioConfig cfg;
    cfg.sampleRate = kRate;
    cfg.channels   = 1;
    cfg.format     = va::audio::AudioFormat::SHORT;

    Collector collector;
    SupertonicVoiceSynthesisProvider tts(onnxDir.string(), stylePath.string(), lang);
    tts.setAudioConfiguration(cfg);
    tts.setVoiceSynthesisResultListener(&collector);
    if (!tts.startVoiceSynthesisProvider()) {
        std::fprintf(stderr, "failed to start provider\n");
        return 1;
    }

    tts.synthesizeSpeech(text);
    {
        std::unique_lock<std::mutex> l(collector.m);
        collector.cv.wait_for(l, std::chrono::minutes(5), [&] { return collector.done; });
    }
    tts.stopVoiceSynthesisProvider();

    if (collector.error || !collector.done) {
        std::fprintf(stderr, "synthesis did not complete\n");
        return 1;
    }

    double sumSq = 0.0;
    int peak = 0;
    for (int16_t s : collector.samples) { sumSq += double(s) * s; peak = std::max(peak, std::abs(int(s))); }
    const double seconds = double(collector.samples.size()) / kRate;
    const double rms     = collector.samples.empty() ? 0.0 : std::sqrt(sumSq / collector.samples.size()) / 32768.0;

    writeWav(outPath, collector.samples, kRate);
    std::printf("lang=%s audio=%.2fs rms=%.4f peak=%d -> %s\n", lang.c_str(), seconds, rms, peak, outPath.c_str());

    // Plausibility: speech-length audio that is not silent and not clipped flat.
    if (seconds < 1.0 || rms < 0.005 || peak >= 32767) {
        std::fprintf(stderr, "implausible audio output\n");
        return 1;
    }
    return 0;
}
