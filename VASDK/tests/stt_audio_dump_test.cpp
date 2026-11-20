// Debugging the STT (diagnostics.stt_audio_dir): each speech segment sent to the recognizer is
// written as a WAV file holding exactly the audio it got, whether it is fed as the user speaks
// (streaming) or in one block when they stop; a private answer's audio is never written.

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/WavFile.h>

#include "TestSupport.h"
#include "engine_test_fakes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

using namespace va;

namespace {

constexpr int kRate = 16000;
constexpr int kFrame = 1600;   // 100 ms

// Takes a whole segment too (writeFullAudioData), as Whistle does when not streaming.
class BatchStt : public fakes::FakeStt {
public:
    audio::TranscriptionResult writeFullAudioData(const uint8_t*, size_t) override { return {}; }
};

class SttAudioDumpTest : public ::testing::Test {
protected:
    va::test::UniqueTemporaryDirectory directory{"vasdk_stt_audio_dump"};
    std::shared_ptr<BatchStt> stt = std::make_shared<BatchStt>();
    std::shared_ptr<AudioRingBuffer> ring = std::make_shared<AudioRingBuffer>(kRate * 10, static_cast<int>(sizeof(short)));
    std::vector<short> spoken;   // everything written to the ring, in order

    void SetUp() override {
        audio::AudioConfig config{};
        config.sampleRate = kRate;
        config.format = audio::AudioFormat::SHORT;
        config.channels = 1;
        stt->setAudioConfiguration(config);
    }
    std::unique_ptr<pipeline::TranscriptionFrameProcessor> processor(bool streaming, bool dumping = true) {
        auto transcription = std::make_unique<pipeline::TranscriptionFrameProcessor>(stt);
        transcription->setStreamingMode(streaming);
        transcription->setRingBuffer(ring);
        if (dumping) transcription->setAudioDumpDirectory(directory.path().string());
        return transcription;
    }
    // 100 ms of a recognisable ramp into the ring; returns where it starts
    uint64_t speak() {
        const uint64_t start = spoken.size();
        std::vector<short> frame(kFrame);
        for (int i = 0; i < kFrame; ++i) frame[i] = static_cast<short>((start + i) % 30000);
        ring->write(reinterpret_cast<const char*>(frame.data()), 0, kFrame);
        spoken.insert(spoken.end(), frame.begin(), frame.end());
        return start;
    }
    std::vector<short> spokenBetween(uint64_t start, uint64_t length) const {
        return {spoken.begin() + static_cast<std::ptrdiff_t>(start), spoken.begin() + static_cast<std::ptrdiff_t>(start + length)};
    }
    static void send(pipeline::TranscriptionFrameProcessor& transcription, std::shared_ptr<transport::Frame> frame) {
        transcription.processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    }
    static std::shared_ptr<transport::VADStateFrame> vad(transport::VADState state, uint64_t start, uint64_t length) {
        auto frame = std::make_shared<transport::VADStateFrame>();
        frame->state = state;
        frame->speechStartOffset = start;
        frame->totalSpeechLength = length;
        return frame;
    }
    static std::shared_ptr<transport::AudioFrame> audioAt(uint64_t offset) {
        auto frame = std::make_shared<transport::AudioFrame>();
        frame->sampleRate = kRate;
        frame->numSamples = kFrame;
        frame->sampleSizeInBytes = sizeof(short);
        frame->ringbufferOffset = offset;
        return frame;
    }
    std::vector<std::filesystem::path> written() const {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path())) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        return files;
    }
    static std::vector<short> samplesOf(const std::filesystem::path& file) {
        audio::WavAudio wav;
        std::string error;
        EXPECT_TRUE(audio::readWav(file.string(), wav, &error)) << error;
        EXPECT_EQ(wav.sampleRate, kRate);
        EXPECT_EQ(wav.channels, 1);
        return audio::toPcm16(wav.samples.data(), wav.samples.size());
    }
};

} // namespace

TEST_F(SttAudioDumpTest, ABlockSentWhenTheUserStopsIsWrittenAsIs) {
    auto transcription = processor(false);
    const uint64_t start = speak();
    speak();
    send(*transcription, vad(transport::VADState::ENDED, start, 2 * kFrame));
    const auto files = written();
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].extension(), ".wav");
    EXPECT_EQ(files[0].filename().string().rfind("stt-", 0), 0u) << files[0];
    EXPECT_EQ(samplesOf(files[0]), spoken) << "the samples the recognizer got";
}

TEST_F(SttAudioDumpTest, StreamedAudioIsWrittenOncePerSegment) {
    auto transcription = processor(true);
    const uint64_t lookback = speak();   // before the VAD was sure: sent as the pre-speech context
    send(*transcription, vad(transport::VADState::STARTING, lookback, kFrame));
    send(*transcription, audioAt(speak()));
    send(*transcription, audioAt(speak()));
    send(*transcription, vad(transport::VADState::ENDED, lookback, 3 * kFrame));
    auto files = written();
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(samplesOf(files[0]), spokenBetween(lookback, 3 * kFrame)) << "lookback and frames, contiguous";

    const uint64_t next = speak();
    send(*transcription, vad(transport::VADState::STARTING, next, kFrame));
    send(*transcription, audioAt(speak()));
    send(*transcription, vad(transport::VADState::ENDED, next, 2 * kFrame));
    files = written();
    ASSERT_EQ(files.size(), 2u) << "the next segment, its own file";
    EXPECT_EQ(samplesOf(files[1]), spokenBetween(next, 2 * kFrame)) << "only its own audio";
}

TEST_F(SttAudioDumpTest, SpeechDroppedBeforeItEndsIsNotWritten) {
    auto transcription = processor(true);
    const uint64_t start = speak();
    send(*transcription, vad(transport::VADState::STARTING, start, kFrame));
    send(*transcription, audioAt(speak()));
    send(*transcription, vad(transport::VADState::QUIET, start, 0));   // too short: never transcribed
    EXPECT_TRUE(written().empty());
}

TEST_F(SttAudioDumpTest, APrivateAnswerIsNeverWritten) {
    auto transcription = processor(false);
    auto policy = std::make_shared<transport::TurnPolicyFrame>();
    policy->privateAnswer = true;   // the agent asked for a card number
    transcription->processFrame(policy, pipeline::FlowDirection::UPSTREAM);
    const uint64_t start = speak();
    send(*transcription, vad(transport::VADState::ENDED, start, kFrame));
    EXPECT_TRUE(written().empty());
}

TEST_F(SttAudioDumpTest, NothingIsWrittenUnlessAsked) {
    auto transcription = processor(false, false);
    const uint64_t start = speak();
    send(*transcription, vad(transport::VADState::ENDED, start, kFrame));
    EXPECT_TRUE(written().empty());
}
