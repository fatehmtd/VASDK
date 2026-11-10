// turn-1-mini: the stream against the reference's scores (tests/gen_scores_golden.py), its state
// handling, and Turn1MiniProvider's decisions around the VAD's states.
//
//   turn1mini_test <turn_1_mini.onnx> <golden/scores_golden.txt>     exit 77 without the model
#include "Turn1MiniProvider.h"
#include "Turn1MiniStream.h"

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/Frame.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using va::thirdparty::turn::Turn1MiniOptions;
using va::thirdparty::turn::Turn1MiniProvider;
using va::thirdparty::turn::Turn1MiniStream;
using Decision = va::audio::TurnDetectionProvider::Decision;

constexpr int kRate = 16000;
constexpr int kSteps = 30;
constexpr double kSpeechSeconds = 3.0;
// The golden comes from the same ONNX Runtime version (gen_scores_golden.py): the same scores on x86-64.
// The int8 graph's integer kernels differ by CPU and by version: 0.0046 on Arm (Pixel 9 Pro XL),
// 0.0106 on a GitHub linux-x64 runner, 0.0012 for ONNX Runtime 1.31. A state handled wrong moves
// the scores by about 0.4, far beyond either bound.
constexpr float kTolerance = 3e-2f;
constexpr float kStateEffect = 0.2f;

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Same as signal() in gen_scores_golden.py: a voice for 3 s (10 harmonics, 4 syllables a second, pitch
// falling over the last 0.6 s) on a faint noise floor, then the noise floor alone
std::vector<float> signal() {
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(kSteps) * Turn1MiniStream::kStepSamples);
    double phase = 0.0;
    unsigned long long seed = 1;
    const double pi = std::acos(-1.0);
    for (int i = 0; i < kSteps * Turn1MiniStream::kStepSamples; ++i) {
        const double t = static_cast<double>(i) / kRate;
        seed = (1103515245ULL * seed + 12345ULL) % 2147483648ULL;
        double value = (static_cast<double>(seed) / 2147483648.0 - 0.5) * 0.002;
        if (t < kSpeechSeconds) {
            const double f0 = t < 2.4 ? 140.0 : 140.0 - 50.0 * (t - 2.4) / 0.6;
            phase += 2.0 * pi * f0 / kRate;
            const double envelope = 0.5 * (1.0 - std::cos(2.0 * pi * 4.0 * t));
            double voice = 0.0;
            for (int k = 1; k <= 10; ++k) voice += std::sin(k * phase) / k;
            value += 0.3 * envelope * voice;
        }
        samples.push_back(static_cast<float>(value));
    }
    return samples;
}

std::vector<float> readGolden(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::vector<float> scores;
    size_t count = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (count == 0) {
            count = std::stoul(line);
            continue;
        }
        scores.push_back(std::stof(line));
    }
    if (scores.size() != count) scores.clear();
    return scores;
}

std::vector<float> run(Turn1MiniStream& stream, const std::vector<float>& audio, size_t chunk) {
    std::vector<float> scores;
    for (size_t at = 0; at < audio.size(); at += chunk)
        stream.push(audio.data() + at, std::min(chunk, audio.size() - at), &scores);
    return scores;
}

float largestDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1.0f;
    float largest = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) largest = std::max(largest, std::fabs(a[i] - b[i]));
    return largest;
}

// ---- The provider, fed as TurnDetectionProcessor feeds it: VAD states and offset-only audio frames ----

struct Feed {
    std::shared_ptr<va::AudioRingBuffer> ring = std::make_shared<va::AudioRingBuffer>(30 * kRate, static_cast<int>(sizeof(short)));
    uint64_t absolute = 0;

    std::shared_ptr<va::transport::AudioFrame> audio(const std::vector<float>& samples, size_t at, size_t count) {
        std::vector<short> pcm(count);
        for (size_t i = 0; i < count; ++i)
            pcm[i] = static_cast<short>(std::clamp(samples[at + i], -1.0f, 1.0f) * 32767.0f);
        auto frame = std::make_shared<va::transport::AudioFrame>();
        frame->ringbufferOffset = static_cast<uint64_t>(ring->getOffset());
        ring->write(reinterpret_cast<const char*>(pcm.data()), 0, static_cast<int>(count));
        frame->numSamples = static_cast<uint32_t>(count);
        frame->sampleRate = kRate;
        frame->channels = 1;
        frame->sampleSizeInBytes = sizeof(short);
        frame->format = static_cast<int>(va::audio::AudioFormat::SHORT);
        frame->absoluteOffset = absolute;
        absolute += count;
        return frame;
    }
};

std::shared_ptr<va::transport::VADStateFrame> vad(va::transport::VADState state) {
    auto frame = std::make_shared<va::transport::VADStateFrame>();
    frame->state = state;
    frame->confidence = 1.0f;
    return frame;
}

va::audio::AudioConfig mono16k() { return {kRate, va::audio::AudioFormat::SHORT, 1}; }

// Plays the signal in 100 ms frames: STARTING at 0, PAUSED after the voice, ENDING 0.5 s later, ENDED
// 0.5 s after that. Returns the time (s) of the first TURN_END and whether it came during the pause.
struct Played {
    double endAt = -1.0;
    bool inPause = false;
    va::transport::TurnStateFrame::TurnType startType = va::transport::TurnStateFrame::TurnType::NORMAL;
};

Played play(Turn1MiniProvider& provider, Feed& feed, const std::vector<float>& audio, bool agentIsSpeaking) {
    constexpr size_t kFrame = kRate / 10;
    Played played;
    auto state = va::transport::VADState::QUIET;
    for (size_t at = 0; at + kFrame <= audio.size(); at += kFrame) {
        const double time = static_cast<double>(at + kFrame) / kRate;
        auto next = state;
        if (state == va::transport::VADState::QUIET) next = va::transport::VADState::STARTING;
        else if (state == va::transport::VADState::STARTING) next = va::transport::VADState::ONGOING;
        if (next == va::transport::VADState::ONGOING && time > kSpeechSeconds) next = va::transport::VADState::PAUSED;
        if (next == va::transport::VADState::PAUSED && time >= kSpeechSeconds + 0.5) next = va::transport::VADState::ENDING;
        if (next == va::transport::VADState::ENDING && time >= kSpeechSeconds + 1.0) next = va::transport::VADState::ENDED;
        if (next != state) {
            state = next;
            const auto evaluation = provider.evaluate(vad(state), agentIsSpeaking);
            if (evaluation.decision == Decision::TURN_START) played.startType = evaluation.type;
        }
        const auto evaluation = provider.evaluate(feed.audio(audio, at, kFrame), agentIsSpeaking);
        if (evaluation.decision == Decision::TURN_END && played.endAt < 0) {
            played.endAt = time;
            played.inPause = state == va::transport::VADState::PAUSED || state == va::transport::VADState::ENDING;
        }
    }
    return played;
}

std::unique_ptr<Turn1MiniProvider> provider(const std::string& model, Feed& feed, float threshold, float pauseThreshold,
                                            bool evaluateOnPause) {
    Turn1MiniOptions options;
    options.modelPath = model;
    options.threshold = threshold;
    options.pauseThreshold = pauseThreshold;
    options.evaluateOnPause = evaluateOnPause;
    auto created = std::make_unique<Turn1MiniProvider>(options);
    if (!created->initialize(mono16k(), feed.ring)) return nullptr;
    return created;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: turn1mini_test <turn_1_mini.onnx> <scores_golden.txt>\n");
        return 2;
    }
    const std::string model = argv[1];
    if (!std::ifstream(model).good()) {
        std::printf("turn-1-mini: no model at %s, skipped\n", model.c_str());
        return 77;
    }
    const auto golden = readGolden(argv[2]);
    if (golden.size() != static_cast<size_t>(kSteps * Turn1MiniStream::kScoresPerStep)) {
        std::printf("can't read %s\n", argv[2]);
        return 2;
    }
    const auto audio = signal();

    // ---- The stream ----
    Turn1MiniStream stream(model);
    if (!stream.initialize()) {
        std::printf("FAIL the model doesn't load\n");
        return 1;
    }
    const auto chunked = run(stream, audio, 1000);
    const float parity = largestDifference(chunked, golden);
    check(parity < kTolerance, "scores match the reference streaming loop (largest difference " + std::to_string(parity) + ")");

    stream.reset();
    const auto whole = run(stream, audio, audio.size());
    check(whole == chunked, "1,000-sample chunks give the same scores as one push");

    stream.reset();
    const auto again = run(stream, audio, Turn1MiniStream::kStepSamples);
    check(again == whole, "reset() starts a stream as fresh as a new one");

    // A score between two steps doesn't touch the stream's state
    stream.reset();
    std::vector<float> peeked;
    for (size_t at = 0; at < audio.size(); at += 1600) {
        stream.push(audio.data() + at, std::min<size_t>(1600, audio.size() - at), &peeked);
        stream.scoreNow();
    }
    check(peeked == whole, "scoreNow() between steps leaves the stream's scores unchanged");
    check(stream.pendingSamples() == 0 && stream.scoreNow() == stream.lastScore(), "with nothing pending, scoreNow() is the last score");

    // The state carries the context: without it, the scores move
    std::vector<float> stateless;
    for (size_t at = 0; at < audio.size(); at += Turn1MiniStream::kStepSamples) {
        stream.reset();
        stream.push(audio.data() + at, Turn1MiniStream::kStepSamples, &stateless);
    }
    const float withoutState = largestDifference(stateless, golden);
    check(withoutState > kStateEffect, "dropping the state changes the scores (by " + std::to_string(withoutState) + ")");

    // Time per step, one thread
    stream.reset();
    std::vector<double> milliseconds;
    for (size_t at = 0; at < audio.size(); at += Turn1MiniStream::kStepSamples) {
        const auto start = std::chrono::steady_clock::now();
        stream.push(audio.data() + at, Turn1MiniStream::kStepSamples);
        milliseconds.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(milliseconds.begin() + 1, milliseconds.end());   // the first step warms up
    std::printf("time per 160 ms step: median %.2f ms, slowest %.2f ms (first %.2f ms)\n",
                milliseconds[milliseconds.size() / 2], milliseconds.back(), milliseconds.front());

    // ---- The provider ----
    {
        Feed feed;
        Turn1MiniProvider wrongRate(Turn1MiniOptions{model});
        check(!wrongRate.initialize({8000, va::audio::AudioFormat::SHORT, 1}, feed.ring), "8 kHz audio is refused");
        Turn1MiniProvider noModel(Turn1MiniOptions{"no/such/turn_1_mini.onnx"});
        check(!noModel.initialize(mono16k(), feed.ring), "a missing model is refused");
    }
    {
        Feed feed;
        auto turn = provider(model, feed, 1.0f, 0.0f, true);
        const auto played = turn ? play(*turn, feed, audio, false) : Played{};
        check(turn && played.inPause && played.endAt > kSpeechSeconds && played.endAt <= kSpeechSeconds + 0.2,
              "a pause threshold of 0 ends the turn on the pause's first frame");
        check(played.startType == va::transport::TurnStateFrame::TurnType::NORMAL, "the user starts a NORMAL turn");
    }
    {
        Feed feed;
        auto turn = provider(model, feed, 0.0f, 0.0f, false);
        const auto played = turn ? play(*turn, feed, audio, true) : Played{};
        check(turn && !played.inPause && played.endAt >= kSpeechSeconds + 1.0 && played.endAt <= kSpeechSeconds + 1.2,
              "without evaluate_on_pause, the turn ends only after the VAD's end");
        check(played.startType == va::transport::TurnStateFrame::TurnType::INTERRUPT, "speech over the agent starts an INTERRUPT");
    }
    {
        Feed feed;
        auto turn = provider(model, feed, 1.0f, 1.0f, true);
        const auto played = turn ? play(*turn, feed, audio, false) : Played{};
        check(turn && played.endAt < 0, "thresholds of 1 never end the turn early: the VAD's end does");
    }

    std::printf("turn-1-mini: %s\n", failures ? "failed" : "all passed");
    return failures ? 1 : 0;
}
