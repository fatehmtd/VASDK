// End-to-end smoke test for MoonshineSttProvider: streams a 16 kHz mono int16 WAV through
// writeAudioData() in 20 ms frames (as the pipeline does), then checks the partials and the
// final from commitEndOfSpeech(), and the one-shot writeFullAudioData() path.
//
// Usage: moonshine_stt_stream_test <model_dir> <speech.wav> [expected words, case-insensitive]
// Exits 77 (skipped) when the model files or the WAV are not present.

#include "MoonshineSTT/MoonshineSttProvider.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Collector : va::audio::TranscriptionResultListener {
    std::mutex              m;
    std::condition_variable cv;
    int                     partials = 0;
    std::string             final;
    bool                    gotFinal = false;

    void onTranscriptionStarted() override {}
    void onTranscriptionStopped() override {}
    void onTranscriptionError(const std::string& e) override { std::fprintf(stderr, "error: %s\n", e.c_str()); }
    void onPartialTranscription(const va::audio::TranscriptionResult& r) override {
        std::lock_guard<std::mutex> l(m);
        ++partials;
        std::printf("partial: %s\n", r.text.c_str());
    }
    void onFinalTranscription(const va::audio::TranscriptionResult& r) override {
        std::lock_guard<std::mutex> l(m);
        final = r.text; gotFinal = true; cv.notify_all();
    }
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Reads the data chunk of a PCM WAV; returns empty on anything unexpected.
std::vector<uint8_t> readWavData(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    for (size_t pos = 12; pos + 8 <= bytes.size();) {
        const uint32_t len = bytes[pos + 4] | bytes[pos + 5] << 8 | bytes[pos + 6] << 16 | uint32_t(bytes[pos + 7]) << 24;
        if (std::equal(bytes.begin() + pos, bytes.begin() + pos + 4, "data"))
            return { bytes.begin() + pos + 8, bytes.begin() + std::min<size_t>(pos + 8 + size_t(len), bytes.size()) };
        pos += 8 + size_t(len) + (len & 1);
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model_dir> <speech.wav> [expected words]\n", argv[0]);
        return 2;
    }
    const std::string modelDir = argv[1], wavPath = argv[2];
    const std::string expected = lower(argc > 3 ? argv[3] : "");

    if (!std::filesystem::exists(std::filesystem::path(modelDir) / "decoder_kv.ort") ||
        !std::filesystem::exists(wavPath)) {
        std::printf("Moonshine model files or %s not found - skipped\n", wavPath.c_str());
        return 77;
    }
    const std::vector<uint8_t> pcm = readWavData(wavPath);
    if (pcm.empty()) { std::fprintf(stderr, "no PCM data in %s\n", wavPath.c_str()); return 1; }

    va::audio::AudioConfig cfg;
    cfg.sampleRate = 16000;
    cfg.channels   = 1;
    cfg.format     = va::audio::AudioFormat::SHORT;

    Collector collector;
    va::thirdparty::stt::MoonshineSttOptions options;
    options.modelDir = modelDir;
    va::thirdparty::stt::MoonshineSttProvider stt(options);
    stt.setAudioConfiguration(cfg);
    stt.setTranscriptionResultListener(&collector);
    if (!stt.initializeTranscriptionProvider() || !stt.startTranscriptionProvider()) {
        std::fprintf(stderr, "failed to start provider\n");
        return 1;
    }

    const size_t frameBytes = 320 * sizeof(int16_t);  // 20 ms
    for (size_t off = 0; off < pcm.size(); off += frameBytes) {
        stt.writeAudioData(pcm.data() + off, std::min(frameBytes, pcm.size() - off));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // faster than real time, still streaming
    }
    stt.commitEndOfSpeech();
    {
        std::unique_lock<std::mutex> l(collector.m);
        collector.cv.wait_for(l, std::chrono::seconds(60), [&] { return collector.gotFinal; });
    }
    stt.stopTranscriptionProvider();

    const auto oneShot = stt.writeFullAudioData(pcm.data(), pcm.size());
    std::printf("final (%d partials): %s\none-shot: %s\n", collector.partials, collector.final.c_str(),
                oneShot.text.c_str());

    bool ok = collector.gotFinal && collector.partials > 0 && !collector.final.empty() && !oneShot.text.empty();
    if (!expected.empty())
        ok = ok && lower(collector.final).find(expected) != std::string::npos &&
             lower(oneShot.text).find(expected) != std::string::npos;
    if (!ok) std::fprintf(stderr, "transcription check failed\n");
    return ok ? 0 : 1;
}
