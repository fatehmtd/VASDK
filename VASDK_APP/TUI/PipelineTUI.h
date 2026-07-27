#pragma once

#ifdef VASDK_HAS_FTXUI

#include <VASDK/FrameProcessor.h>
#include <VASDK/Frame.h>
#include <VizAnim/AudioFeaturesFrame.h>

#include <VizAnim/VizAnimProvider.h>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vasdk_app {

struct AudioDisplayState {
    float rms      = 0.0f;
    float bass     = 0.0f;
    float lowMid   = 0.0f;
    float mid      = 0.0f;
    float hiMid    = 0.0f;
    float treble   = 0.0f;
    float centroid = 0.0f;
    float pitch    = 0.0f;
    float voiced   = 0.0f;
    float zcr      = 0.0f;
    bool  onset    = false;
    std::array<float, 12> chroma{};
};

// Pipeline stage that captures AudioFeaturesFrames, AgentStateFrames, and
// TranscriptionFrames for the TUI to read.  All frames are forwarded unchanged.
class AudioFeaturesObserver : public va::pipeline::FrameProcessor {
public:
    AudioFeaturesObserver();

    AudioFeaturesObserver(const AudioFeaturesObserver&) = delete;
    AudioFeaturesObserver& operator=(const AudioFeaturesObserver&) = delete;

    virtual ~AudioFeaturesObserver() override = default;

    std::string getName() const override { return "AudioFeaturesObserver"; }

    bool shouldProcess(std::shared_ptr<va::transport::Frame> frame) const override;
    void processFrame(std::shared_ptr<va::transport::Frame> frame,
                      va::pipeline::FlowDirection dir) override;

    AudioDisplayState        getMicState()   const;
    AudioDisplayState        getSynthState() const;

    // Called from the MiniAudioSink playback callback (audio thread).
    // Accumulates bytes; extracts features once a full FFT window is available.
    void onSynthPlayback(const char* data, size_t bytes);

    // Must be called before onSynthPlayback; initialises the internal extractor.
    void setSynthAudioConfig(const va::audio::AudioConfig& config);

    std::string              getStatus() const;
    std::vector<std::string> getLogs()   const;

    void appendLog(std::string msg);

private:
    static AudioDisplayState toDisplayState(const va::thirdparty::VizAnimProvider::Features& f);

    mutable std::mutex      _mutex;
    AudioDisplayState       _micState;
    AudioDisplayState       _synthState;

    // Playback-driven synth extraction (called from audio thread, no pipeline involvement)
    std::unique_ptr<va::thirdparty::VizAnimProvider> _synthProvider;
    std::vector<char>                                _synthAccum;
    size_t                                           _synthWindowBytes = 0; // FFT_SIZE * bytesPerSample

    std::string             _status = "Initializing...";
    std::deque<std::string> _logs;
    static constexpr size_t kMaxLogs = 8;

    uint64_t _initialTimestamp = 0; // for relative timestamps in logs
};

// Blocking terminal UI.  Call run() after the pipeline is started; it returns
// when the user presses 'q' or when appRunning is set to false externally.
class PipelineTUI {
public:
    explicit PipelineTUI(std::shared_ptr<AudioFeaturesObserver> observer);
    ~PipelineTUI();

    void run(std::atomic<bool>& appRunning);

private:
    std::shared_ptr<AudioFeaturesObserver> _observer;
};

} // namespace vasdk_app

#endif // VASDK_HAS_FTXUI
