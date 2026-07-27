#pragma once

#include "Transport.h"
#include "GenericAudioSource.h"
#include "GenericAudioSink.h"
#include "AudioRingBuffer.h"
#include "metrics/MetricHandles.h"
#include <atomic>
#include <thread>
#include <vector>

namespace va {
namespace transport {

/**
 * @brief Local transport using hardware audio I/O - captures and plays audio
 * from/to local hardware via GenericAudioSource and GenericAudioSink.
 */
class VASDK_EXPORT LocalTransport : public BaseTransport {
public:
    /**
     * @brief Construct LocalTransport
     * @param audioSource Audio input source (e.g., SDLAudioSource)
     * @param audioSink Audio output sink (e.g., SDLAudioSink)
     */
    LocalTransport(
        std::shared_ptr<audio::GenericAudioSource> audioSource,
        std::shared_ptr<audio::GenericAudioSink> audioSink
    );

    virtual ~LocalTransport();

    // Transport interface
    std::string getName() const override { return "LocalTransport"; }
    std::string getDescription() const override {
        return "Local hardware audio transport (SDL/ALSA/etc)";
    }

    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    bool sendFrame(std::shared_ptr<Frame> frame) override;

    /**
     * @brief Set the input audio configuration
     * @param config Audio configuration
     */
    void setInputAudioConfig(const audio::AudioConfig& config) {
        _inputAudioConfig = config;
    }

    /**
     * @brief Get the input audio configuration
     * @return Audio configuration
     */
    audio::AudioConfig getInputAudioConfig() const {
        return _inputAudioConfig;
    }

    /**
     * @brief Set the output audio configuration
     * @param config Audio configuration
     */
    void setOutputAudioConfig(const audio::AudioConfig& config) {
        _outputAudioConfig = config;
    }

    /**
     * @brief Get the output audio configuration
     * @return Audio configuration
     */
    audio::AudioConfig getOutputAudioConfig() const {
        return _outputAudioConfig;
    }

    /**
     * @brief Set the frame size in samples
     * @param samples Number of samples per frame (default: 2000 = 125ms at 16kHz)
     */
    void setFrameSize(size_t samples) {
        _frameSizeInSamples = samples;
    }

    /**
     * @brief Set the ring buffer for audio storage
     * @param ringBuffer Shared ring buffer (if nullptr, internal buffer is used)
     */
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) {
        _ringBuffer = ringBuffer;
    }

    /**
     * @brief Get the ring buffer
     */
    std::shared_ptr<AudioRingBuffer> getRingBuffer() const { return _ringBuffer; }

    /**
     * @brief Returns the number of bytes queued in the audio sink but not yet played.
     */
    int getPendingOutputAudioBytes() const {
        return _audioSink ? _audioSink->getPendingAudioBytes() : 0;
    }

    /**
     * @brief Gracefully fade out and discard buffered output audio (barge-in / interrupt).
     *        Applies a ~50 ms linear ramp-down to prevent an abrupt click.
     */
    void flushOutputAudio() {
        if (_audioSink) _audioSink->fadeAndFlush();
    }

private:
    /**
     * @brief Audio capture thread function
     */
    void captureThreadFunc();

    /**
     * @brief Convert raw audio buffer to AudioFrame
     */
    std::shared_ptr<AudioFrame> bufferToFrame(const char*buffer, uint32_t numSamples);

    // Audio components
    std::shared_ptr<audio::GenericAudioSource> _audioSource;
    std::shared_ptr<audio::GenericAudioSink> _audioSink;
    audio::AudioConfig _inputAudioConfig;
    audio::AudioConfig _outputAudioConfig;

    // Capture thread
    std::thread _captureThread;
    std::atomic<bool> _captureRunning;

    // Frame configuration
    size_t _frameSizeInSamples;  // Number of samples per frame
    uint64_t _absoluteOffset;    // Current absolute position in stream

    // Sequence counter
    std::atomic<uint64_t> _sequenceCounter;

    // Ring buffer for audio storage
    std::shared_ptr<AudioRingBuffer>     _ringBuffer;
    std::vector<char>                    _outputBuffer;  // Buffer for reading from ring buffer for output

    // Metrics
    va::metrics::SamplerHandle _callbackJitterSampler;
    va::metrics::CounterHandle _underrunCounter;
    std::chrono::steady_clock::time_point _lastCaptureTime;
    std::atomic<bool> _firstCapture{true};
};

} // namespace transport
} // namespace va
