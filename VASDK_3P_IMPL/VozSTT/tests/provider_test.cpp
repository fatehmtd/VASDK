// Drives VozTranscriptionProvider with a fake recognizer (the real one lives in a platform SDK).
#include "VozSTT/VozTranscriptionProvider.hpp"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RecordingListener : va::audio::TranscriptionResultListener {
    std::mutex mutex;
    std::vector<va::audio::TranscriptionResult> finals;
    std::vector<std::string> errors;
    void onTranscriptionStarted() override {}
    void onTranscriptionStopped() override {}
    void onTranscriptionError(const std::string& error) override { std::lock_guard<std::mutex> lock(mutex); errors.push_back(error); }
    void onPartialTranscription(const va::audio::TranscriptionResult&) override {}
    void onFinalTranscription(const va::audio::TranscriptionResult& result) override { std::lock_guard<std::mutex> lock(mutex); finals.push_back(result); }
    size_t eventCount() { std::lock_guard<std::mutex> lock(mutex); return finals.size() + errors.size(); }
};

int failures = 0;
void check(bool condition, const char* what)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

bool waitForEvents(RecordingListener& listener, size_t count)
{
    for (int i = 0; i < 200 && listener.eventCount() < count; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return listener.eventCount() >= count;
}

void writeSamples(va::thirdparty::VozTranscriptionProvider& provider, size_t count, int16_t value)
{
    std::vector<int16_t> samples(count, value);
    provider.writeAudioData(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(int16_t));
}

} // namespace

int main()
{
    std::vector<size_t> receivedSizes;
    std::vector<int> receivedRates;
    bool failNext = false;
    va::thirdparty::VozTranscriptionOptions options;
    options.recognizer = [&](const std::vector<float>& samples, int sampleRate) {
        if (failNext) { failNext = false; throw std::runtime_error("model missing"); }
        receivedSizes.push_back(samples.size());
        receivedRates.push_back(sampleRate);
        va::audio::TranscriptionResult result;
        result.text = "hello world";
        result.words = { { 80, 400, "hello" }, { 480, 900, "world" } };
        return result;
    };
    va::thirdparty::VozTranscriptionProvider provider(options);

    va::audio::AudioConfig config{};
    config.sampleRate = 16000;
    config.channels = 1;
    config.format = va::audio::AudioFormat::SHORT;
    provider.setAudioConfiguration(config);

    RecordingListener listener;
    provider.setTranscriptionResultListener(&listener);
    check(provider.initializeTranscriptionProvider(), "initializes");
    check(provider.startTranscriptionProvider(), "starts");

    // An utterance becomes one final at end of speech, timed from its words.
    writeSamples(provider, 16000, 1000);
    provider.commitEndOfSpeech();
    check(waitForEvents(listener, 1), "final arrives after commitEndOfSpeech");
    check(receivedSizes.size() == 1 && receivedSizes[0] == 16000, "recognizer gets the whole utterance");
    check(receivedRates.size() == 1 && receivedRates[0] == 16000, "recognizer gets the sample rate");
    check(listener.finals.size() == 1 && listener.finals[0].text == "hello world", "final text");
    check(listener.finals.size() == 1 && listener.finals[0].startTimeMs == 80 && listener.finals[0].endTimeMs == 900, "final timing");

    // Barge-in: cleared audio never reaches the recognizer.
    writeSamples(provider, 8000, 1000);
    provider.clearAudioBuffer();
    writeSamples(provider, 4000, 1000);
    provider.commitEndOfSpeech();
    check(waitForEvents(listener, 2), "final after barge-in");
    check(receivedSizes.size() == 2 && receivedSizes[1] == 4000, "cleared audio is dropped");

    // A failing recognizer reports an error instead of a final.
    failNext = true;
    writeSamples(provider, 4000, 1000);
    provider.commitEndOfSpeech();
    check(waitForEvents(listener, 3), "error arrives");
    check(listener.errors.size() == 1 && listener.finals.size() == 2, "error, not final");

    provider.stopTranscriptionProvider();
    if (failures == 0) std::printf("voz_stt_provider: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
