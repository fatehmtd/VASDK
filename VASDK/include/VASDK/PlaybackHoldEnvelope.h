#pragma once

#include "Frame.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace va {
namespace audio {

/**
 * @brief Gain envelope that ducks or pauses agent playback while a barge-in is pending.
 *
 * setHold() is called from the pipeline; apply() and isPausedSilent() from the audio
 * thread that hands samples to the device or network. Gain moves linearly toward its
 * target over rampFrameCount frames, so ducking, pausing and resuming never click.
 * In Pause mode, once the gain reaches zero the caller must stop consuming its queue
 * and output silence, so playback resumes from the exact sample where it paused.
 */
class PlaybackHoldEnvelope {
public:
    void setHold(transport::PendingPlaybackMode pendingPlaybackMode, bool held, float duckGainDecibels) {
        if (!held) {
            _pauseRequested = false;
            _targetGain = 1.0f;
            return;
        }
        switch (pendingPlaybackMode) {
        case transport::PendingPlaybackMode::Duck:
            _pauseRequested = false;
            _targetGain = std::pow(10.0f, duckGainDecibels / 20.0f);
            break;
        case transport::PendingPlaybackMode::Pause:
            _pauseRequested = true;
            _targetGain = 0.0f;
            break;
        case transport::PendingPlaybackMode::Continue:
            break;
        }
    }

    /// Back to full volume at once (flush, disconnect, stop).
    void reset() {
        _pauseRequested = false;
        _targetGain = 1.0f;
        _currentGain = 1.0f;
    }

    void setRampFrameCount(size_t rampFrameCount) { _rampFrameCount = std::max<size_t>(1, rampFrameCount); }

    /// True when paused and fully faded out: output silence and leave the queue untouched.
    bool isPausedSilent() const { return _pauseRequested && _currentGain.load() <= 0.0f; }

    bool isUnity() const { return _currentGain.load() == 1.0f && _targetGain.load() == 1.0f; }

    float currentGain() const { return _currentGain.load(); }

    void apply(int16_t* samples, size_t frameCount, int channelCount) {
        applyGain(samples, frameCount, channelCount, [](int16_t sample, float gain) {
            return static_cast<int16_t>(std::lround(static_cast<float>(sample) * gain));
        });
    }

    void apply(int32_t* samples, size_t frameCount, int channelCount) {
        applyGain(samples, frameCount, channelCount, [](int32_t sample, float gain) {
            return static_cast<int32_t>(std::llround(static_cast<double>(sample) * gain));
        });
    }

    void apply(float* samples, size_t frameCount, int channelCount) {
        applyGain(samples, frameCount, channelCount, [](float sample, float gain) { return sample * gain; });
    }

    /// Unsigned 8-bit PCM: silence is 128.
    void apply(uint8_t* samples, size_t frameCount, int channelCount) {
        applyGain(samples, frameCount, channelCount, [](uint8_t sample, float gain) {
            return static_cast<uint8_t>(std::lround((static_cast<float>(sample) - 128.0f) * gain + 128.0f));
        });
    }

private:
    template <typename Sample, typename Scale>
    void applyGain(Sample* samples, size_t frameCount, int channelCount, Scale scale) {
        if (isUnity()) return;
        const float target = _targetGain.load();
        const float step = 1.0f / static_cast<float>(_rampFrameCount);
        float gain = _currentGain.load();
        const size_t channels = static_cast<size_t>(std::max(1, channelCount));
        for (size_t frame = 0; frame < frameCount; ++frame) {
            if (gain < target) gain = std::min(target, gain + step);
            else if (gain > target) gain = std::max(target, gain - step);
            for (size_t channel = 0; channel < channels; ++channel) {
                Sample& sample = samples[frame * channels + channel];
                sample = scale(sample, gain);
            }
        }
        _currentGain = gain;
    }

    std::atomic<float> _targetGain{1.0f};
    std::atomic<float> _currentGain{1.0f};
    std::atomic<bool>  _pauseRequested{false};
    size_t             _rampFrameCount = 320;  // 20 ms at 16 kHz; sinks set their own rate / 50
};

} // namespace audio
} // namespace va
