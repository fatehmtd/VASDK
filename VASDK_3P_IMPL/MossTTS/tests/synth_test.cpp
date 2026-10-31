// End-to-end smoke test for MossVoiceSynthesisProvider: synthesizes text through the full
// ONNX pipeline at the pipeline's 16 kHz int16 format and writes a WAV for listening.
//
// Usage: moss_tts_synth_test <model_dir> [out.wav] [voice] [text | @utf8_text_file]
// Exits 77 (skipped) when the model files are not present.

#include "MossTTS/MossVoiceSynthesisProvider.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
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
    bool done = false, error = false, started = false;
    std::chrono::steady_clock::time_point t0, firstAudio;
    bool gotAudio = false;

    void onSynthesisStarted() override { std::lock_guard<std::mutex> l(m); started = true; }
    void onSynthesisCompleted() override { std::lock_guard<std::mutex> l(m); done = true; cv.notify_all(); }
    void onSynthesisError(const std::string& e) override {
        std::fprintf(stderr, "synthesis error: %s\n", e.c_str());
        std::lock_guard<std::mutex> l(m); error = done = true; cv.notify_all();
    }
    void onAudioDataReceived(const char* data, size_t size) override {
        std::lock_guard<std::mutex> l(m);
        if (!gotAudio) { gotAudio = true; firstAudio = std::chrono::steady_clock::now(); }
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
        std::fprintf(stderr, "usage: %s <model_dir> [out.wav] [voice] [text | @utf8_text_file]\n", argv[0]);
        return 2;
    }
    const std::string modelDir = argv[1];
    const std::string outPath  = argc > 2 ? argv[2] : "moss_tts_test.wav";
    const std::string voice    = argc > 3 ? argv[3] : "Ava";
    std::string text = argc > 4 ? argv[4]
        : "Hello! This is a quick test of the MOSS text to speech engine, running fully offline.";
    // "@file" reads UTF-8 text from a file (Windows argv is not UTF-8).
    if (text.size() > 1 && text[0] == '@') {
        std::ifstream f(text.substr(1), std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), {});
    }

    if (!std::filesystem::exists(std::filesystem::path(modelDir) / "MOSS-TTS-Nano-100M-ONNX" /
                                 "browser_poc_manifest.json")) {
        std::printf("MOSS-TTS model files not found under %s - skipped\n", modelDir.c_str());
        return 77;
    }

    constexpr int kRate = 16000;
    va::audio::AudioConfig cfg;
    cfg.sampleRate = kRate;
    cfg.channels   = 1;
    cfg.format     = va::audio::AudioFormat::SHORT;

    Collector collector;
    MossVoiceSynthesisProvider tts(modelDir, voice);
    tts.setAudioConfiguration(cfg);
    tts.setVoiceSynthesisResultListener(&collector);
    if (!tts.startVoiceSynthesisProvider()) {
        std::fprintf(stderr, "failed to start provider\n");
        return 1;
    }

    collector.t0 = std::chrono::steady_clock::now();
    tts.synthesizeSpeech(text);
    {
        std::unique_lock<std::mutex> l(collector.m);
        collector.cv.wait_for(l, std::chrono::minutes(5), [&] { return collector.done; });
    }
    const auto t1 = std::chrono::steady_clock::now();
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
    const double elapsed = std::chrono::duration<double>(t1 - collector.t0).count();
    const double ttfa    = std::chrono::duration<double>(collector.firstAudio - collector.t0).count();

    writeWav(outPath, collector.samples, kRate);
    std::printf("voice=%s audio=%.2fs elapsed=%.2fs RTF=%.2f first-audio=%.2fs rms=%.4f peak=%d -> %s\n",
                voice.c_str(), seconds, elapsed, seconds > 0 ? elapsed / seconds : 0.0, ttfa, rms, peak,
                outPath.c_str());

    // Plausibility: speech-length audio that is not silent and not clipped flat.
    if (seconds < 1.0 || rms < 0.005 || peak >= 32767) {
        std::fprintf(stderr, "implausible audio output\n");
        return 1;
    }
    return 0;
}
