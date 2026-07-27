#include "PipelineTUI.h"

#ifdef VASDK_HAS_FTXUI

#include <VASDK/Frame.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

using namespace ftxui;

namespace vasdk_app {

    //  AudioFeaturesObserver 

    AudioFeaturesObserver::AudioFeaturesObserver()
    {
        _initialTimestamp = va::transport::Frame::getCurrentTimestamp();
    }

    bool AudioFeaturesObserver::shouldProcess(std::shared_ptr<va::transport::Frame> frame) const {
        auto t = frame->getType();
        return t == va::transport::FrameType::AUDIO_FEATURES
            || t == va::transport::FrameType::AGENT_STATE
            || t == va::transport::FrameType::TRANSCRIPTION
            || t == va::transport::FrameType::SYNTHESIS_STATE
            || t == va::transport::FrameType::TEXT;
    }

    void AudioFeaturesObserver::processFrame(std::shared_ptr<va::transport::Frame> frame,
        va::pipeline::FlowDirection dir) {
        if (dir != va::pipeline::FlowDirection::DOWNSTREAM) {
            pushUpstream(frame);
            return;
        }

        switch (frame->getType()) {
        case va::transport::FrameType::AUDIO_FEATURES: {
            // Only consume MIC features here; synth is driven by onSynthPlayback().
            auto af = std::dynamic_pointer_cast<va::thirdparty::AudioFeaturesFrame>(frame);
            if (af && !af->featuresArray.empty()
                && af->source == va::thirdparty::AudioFeaturesSource::MIC) {
                std::lock_guard<std::mutex> lk(_mutex);
                _micState = toDisplayState(af->featuresArray.back());
            }
            break;
        }
        case va::transport::FrameType::AGENT_STATE: {
            auto asf = std::dynamic_pointer_cast<va::transport::AgentStateFrame>(frame);
            if (asf) {
                std::lock_guard<std::mutex> lk(_mutex);
                _status = va::transport::toString(asf->currentState);
            }
            break;
        }
        case va::transport::FrameType::TEXT: {
            auto tf = std::dynamic_pointer_cast<va::transport::TextFrame>(frame);
            if (tf && !tf->text.empty()) {
                appendLog("[TEXT] \"" + tf->text + "\" " + std::to_string(frame->timestamp - _initialTimestamp));
            }
        } break;
        case va::transport::FrameType::TRANSCRIPTION: {
            auto tf = std::dynamic_pointer_cast<va::transport::TranscriptionFrame>(frame);
            if (tf && tf->isFinal && !tf->transcriptionResult.text.empty()) {
                appendLog("[STT] \"" + tf->transcriptionResult.text + "\" " + std::to_string(frame->timestamp - _initialTimestamp));
            }
        } break;
        case va::transport::FrameType::SYNTHESIS_STATE: {
            auto sf = std::dynamic_pointer_cast<va::transport::SynthesisStateFrame>(frame);
            if (sf && (sf->state == va::transport::SynthesisState::COMPLETED
                || sf->state == va::transport::SynthesisState::INTERRUPTED)) {
                // Playback ended - clear accumulator and zero synth state so bars fade out
                _synthAccum.clear();
                std::lock_guard<std::mutex> lk(_mutex);
                _synthState = AudioDisplayState{};
            }
            break;
        }
        default:
        break;
        }

        pushDownstream(frame);
    }

    AudioDisplayState AudioFeaturesObserver::toDisplayState(const va::thirdparty::VizAnimProvider::Features& f) {
        AudioDisplayState s;
        s.rms = f.rms;
        s.bass = f.bass;
        s.lowMid = f.low_mid;
        s.mid = f.mid;
        s.hiMid = f.hi_mid;
        s.treble = f.treble;
        s.centroid = f.centroid;
        s.pitch = f.pitch;
        s.voiced = f.voiced;
        s.zcr = f.zcr;
        s.onset = f.onset;
        s.chroma = f.chroma;
        return s;
    }

    AudioDisplayState AudioFeaturesObserver::getMicState() const {
        std::lock_guard<std::mutex> lk(_mutex);
        return _micState;
    }

    AudioDisplayState AudioFeaturesObserver::getSynthState() const {
        std::lock_guard<std::mutex> lk(_mutex);
        return _synthState;
    }

    void AudioFeaturesObserver::setSynthAudioConfig(const va::audio::AudioConfig& config) {
        _synthProvider = std::make_unique<va::thirdparty::VizAnimProvider>();
        _synthProvider->initialize(config);
        _synthWindowBytes = static_cast<size_t>(va::thirdparty::VizAnimProvider::FFT_SIZE)
            * config.getSampleSizeInBytes();
        _synthAccum.clear();
        _synthAccum.reserve(_synthWindowBytes * 2);
    }

    void AudioFeaturesObserver::onSynthPlayback(const char* data, size_t bytes) {
        if (!_synthProvider || _synthWindowBytes == 0) return;

        _synthAccum.insert(_synthAccum.end(), data, data + bytes);

        // Extract one feature per full FFT window; leave remainder in the accumulator
        while (_synthAccum.size() >= _synthWindowBytes) {
            auto features = _synthProvider->computeFeatures(
                _synthAccum.data(), static_cast<int>(va::thirdparty::VizAnimProvider::FFT_SIZE));

            if (!features.empty()) {
                std::lock_guard<std::mutex> lk(_mutex);
                _synthState = toDisplayState(features.back());
            }
            _synthAccum.erase(_synthAccum.begin(),
                _synthAccum.begin() + static_cast<std::ptrdiff_t>(_synthWindowBytes));
        }
    }

    // No explicit reset needed on SYNTHESIS_STATE COMPLETED/INTERRUPTED;
    // smooth decay already brings synth state to zero on its own.

    std::string AudioFeaturesObserver::getStatus() const {
        std::lock_guard<std::mutex> lk(_mutex);
        return _status;
    }

    void AudioFeaturesObserver::appendLog(std::string msg) {
        std::lock_guard<std::mutex> lk(_mutex);
        _logs.push_back(std::move(msg));
        while (_logs.size() > kMaxLogs)
            _logs.pop_front();
    }

    std::vector<std::string> AudioFeaturesObserver::getLogs() const {
        std::lock_guard<std::mutex> lk(_mutex);
        return { _logs.begin(), _logs.end() };
    }

    //  Rendering helpers 

    // Map a raw feature value to [0,1] with a dB log scale.
    // floorDb is the dB level (negative) that maps to 0.  Values at ≥ 0 dB map to 1.
    static float scaleDb(float v, float floorDb) {
        if (v <= 0.0f) return 0.0f;
        float db = 20.0f * std::log10f(std::max(v, 1e-9f));
        return std::clamp((db - floorDb) / (-floorDb), 0.0f, 1.0f);
    }

    // RMS: amplitude in [0,1] after int16→float.  Speech ≈ 0.01–0.2 (−40 to −14 dB).
    static float scaleRMS(float v) { return scaleDb(v, -60.0f); }

    // Band energies: avg |X[k]|/N over a freq band.  Speech ≈ 0.0001–0.01 (−80 to −40 dB).
    static float scaleBand(float v) { return scaleDb(v, -90.0f); }

    // Exponential smoothing: advance smooth toward raw by alpha each frame
    static void smoothToward(AudioDisplayState& s, const AudioDisplayState& r, float a) {
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        s.rms = lerp(s.rms, r.rms, a);
        s.bass = lerp(s.bass, r.bass, a);
        s.lowMid = lerp(s.lowMid, r.lowMid, a);
        s.mid = lerp(s.mid, r.mid, a);
        s.hiMid = lerp(s.hiMid, r.hiMid, a);
        s.treble = lerp(s.treble, r.treble, a);
        s.centroid = lerp(s.centroid, r.centroid, a);
        s.pitch = lerp(s.pitch, r.pitch, a);
        s.voiced = lerp(s.voiced, r.voiced, a);
        s.zcr = lerp(s.zcr, r.zcr, a);
        s.onset = r.onset;
        for (int i = 0; i < 12; ++i)
            s.chroma[i] = lerp(s.chroma[i], r.chroma[i], a);
    }

    // Renders one gauge row.  isRms selects the amplitude dB scale; otherwise band scale.
    static Element makeBandRow(const char* label, float value, Color baseColor, bool onset, bool isRms = false) {
        float v = isRms ? scaleRMS(value) : scaleBand(value);

        Color col = baseColor;
        if (onset)          col = Color::Yellow;
        else if (v > 0.85f) col = Color::RedLight;
        else if (v > 0.60f) col = Color::YellowLight;

        auto bar = gauge(v) | color(col);
        return hbox({
            text(label) | size(WIDTH, EQUAL, 7) | dim,
            bar | flex,
            text(std::to_string(static_cast<int>(v * 100)) + "%") | size(WIDTH, EQUAL, 5),
            });
    }

    // Spectrum panel for one source (mic or synth)
    static Element makeSpectrumPanel(const AudioDisplayState& s,
        const std::string& title,
        Color col) {
        auto pitchStr = s.pitch > 20.0f
            ? std::to_string(static_cast<int>(s.pitch)) + " Hz"
            : "---";

        return window(
            text(" " + title + " ") | bold,
            vbox({
                makeBandRow("Bass   ", s.bass,   col, s.onset),
                makeBandRow("LowMid ", s.lowMid, col, s.onset),
                makeBandRow("Mid    ", s.mid,    col, s.onset),
                makeBandRow("HiMid  ", s.hiMid,  col, s.onset),
                makeBandRow("Treble ", s.treble, col, s.onset),
                makeBandRow("RMS    ", s.rms,    col, s.onset, /*isRms=*/true),
                separator(),
                hbox({
                    text("Pitch: ") | dim,
                    text(pitchStr) | bold,
                    text("  ZCR: ") | dim,
                    text(std::to_string(static_cast<int>(s.zcr * 100)) + "%"),
                    text("  Onset: ") | dim,
                    text(s.onset ? "●" : "○") | color(s.onset ? Color::Yellow : Color::GrayDark),
                }),
                })
                ) | flex;
    }

    // Chromagram: 12 mini horizontal gauges, two rows (mic above, synth below)
    static Element makeChromaPanel(const AudioDisplayState& mic,
        const AudioDisplayState& synth) {
        static constexpr const char* kNotes[12] = {
            "C ", "C#", "D ", "D#", "E ", "F ",
            "F#", "G ", "G#", "A ", "A#", "B "
        };

        Elements rows;
        // Label row
        {
            Elements cells;
            for (auto* n : kNotes)
                cells.push_back(text(n) | size(WIDTH, EQUAL, 5) | center | dim);
            rows.push_back(hbox(std::move(cells)));
        }
        // Mic chroma
        {
            Elements cells;
            for (int i = 0; i < 12; ++i)
                cells.push_back(
                    gauge(mic.chroma[i]) | color(Color::Green) | size(WIDTH, EQUAL, 5));
            rows.push_back(hbox(std::move(cells)));
        }
        // Synth chroma
        {
            Elements cells;
            for (int i = 0; i < 12; ++i)
                cells.push_back(
                    gauge(synth.chroma[i]) | color(Color::Cyan) | size(WIDTH, EQUAL, 5));
            rows.push_back(hbox(std::move(cells)));
        }
        // Legend
        rows.push_back(
            hbox({
                text("■ Mic") | color(Color::Green) | dim,
                text("  ■ Synth") | color(Color::Cyan) | dim,
                })
                );

        return window(text(" Chromagram "), vbox(std::move(rows)));
    }

    // Status colour per agent state string
    static Color statusColour(const std::string& s) {
        if (s == "LISTENING")   return Color::Green;
        if (s == "THINKING")    return Color::Yellow;
        if (s == "SPEAKING")    return Color::Cyan;
        if (s == "INTERRUPTED") return Color::RedLight;
        return Color::GrayLight;
    }

    // Animated "wave" glyph cycling with time - decorative indicator when active
    static const char* waveGlyph(int tick, bool active) {
        static constexpr const char* kFrames[] = { "▁","▂","▃","▄","▅","▆","▇","█","▇","▆","▅","▄","▃","▂" };
        if (!active) return " ";
        return kFrames[tick % 14];
    }

    //  PipelineTUI::run 

    PipelineTUI::PipelineTUI(std::shared_ptr<AudioFeaturesObserver> observer)
        : _observer(std::move(observer)) {
    }

    PipelineTUI::~PipelineTUI() = default;

    void PipelineTUI::run(std::atomic<bool>& appRunning) {
        auto screen = ScreenInteractive::TerminalOutput();

        AudioDisplayState smoothMic, smoothSynth;
        int tick = 0;

        auto renderer = Renderer([&] {
            ++tick;
            // Smooth raw features toward display state
            smoothToward(smoothMic, _observer->getMicState(), 0.25f);
            smoothToward(smoothSynth, _observer->getSynthState(), 0.25f);

            auto status = _observer->getStatus();
            auto logs = _observer->getLogs();

            bool micActive = smoothMic.rms > 0.0005f;
            bool synthActive = smoothSynth.rms > 0.0005f;

            // Title bar with animated wave for whichever channel is active
            auto title = hbox({
                text(" VASDK Voice Assistant") | bold,
                filler(),
                text(waveGlyph(tick, micActive)) | color(Color::Green),
                text(waveGlyph(tick + 7, micActive)) | color(Color::GreenLight),
                text("  "),
                text(waveGlyph(tick, synthActive)) | color(Color::Cyan),
                text(waveGlyph(tick + 7, synthActive)) | color(Color::CyanLight),
                text(" "),
                }) | bgcolor(Color::NavyBlue);

            // Side-by-side spectrum panels
            auto spectra = hbox({
                makeSpectrumPanel(smoothMic,   "MIC INPUT",    Color::Green),
                makeSpectrumPanel(smoothSynth, "SYNTH OUTPUT", Color::Cyan),
                });

            // Chromagram
            auto chroma = makeChromaPanel(smoothMic, smoothSynth);

            // Status bar
            auto statusBar = hbox({
                text(" Status: ") | bold,
                text(status) | bold | color(statusColour(status)),
                filler(),
                text("Press 'q' to quit ") | dim,
                }) | border;

            // Log panel (last N lines)
            Elements logLines;
            for (const auto& l : logs)
                logLines.push_back(paragraph(l));
            if (logLines.empty())
                logLines.push_back(text("(no log entries yet)") | dim);
            auto logPanel = window(text(" Log "), vbox(std::move(logLines)));

            return vbox({
                title,
                spectra,
                chroma,
                statusBar,
                logPanel,
                });
            });

        // Catch 'q' to signal shutdown
        auto root = CatchEvent(renderer, [&](Event e) {
            if (e == Event::Character('q') || e == Event::Character('Q')) {
                appRunning = false;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
            });

        // Background thread: post a custom event at ~30 fps to drive animation
        std::thread refresher([&] {
            while (appRunning) {
                screen.PostEvent(Event::Custom);
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            screen.ExitLoopClosure()();
            });

        screen.Loop(root);

        // Ensure the outer loop also stops if the user quit from the TUI
        appRunning = false;
        refresher.join();
    }

} // namespace vasdk_app

#endif // VASDK_HAS_FTXUI
