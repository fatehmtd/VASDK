#pragma once

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/Frame.h>
#include <VASDK/Transport.h>
#include "vasdk_ffmpeg_audio_export.h"

#include "FFmpegAudioSource.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

/**
 * @brief Transport that feeds the pipeline with audio decoded from a file.
 *
 * FileTransport is an input-only transport that wraps FFmpegAudioSource.
 * It opens the file, decodes and resamples the audio into the requested
 * AudioConfig, and emits AudioFrames at a real-time pace through the standard
 * BaseTransport frame queue consumed by TransportInputProcessor.
 *
 * Any audio format or codec that FFmpeg supports is accepted
 * (MP3, FLAC, WAV, OGG, M4A, OPUS, AAC, WMA, …).
 *
 * sendFrame() is a no-op because this transport has no audio output sink.
 * Connect an output transport (e.g., LocalTransport) for TTS playback.
 *
 * EOF handling
 * ------------
 * When the file reaches the end of its audio data:
 *  1. The capture thread stops enqueuing new frames.
 *  2. The @c onEndOfFile callback is invoked (from the capture thread).
 *  3. Calling stop() or release() from within onEndOfFile is safe.
 *
 * @code
 *   va::audio::AudioConfig cfg{16000, va::audio::AudioFormat::SHORT, 1};
 *
 *   auto transport = std::make_shared<FileTransport>("/path/to/clip.flac");
 *   transport->setAudioConfig(cfg);
 *   transport->setRingBuffer(myRingBuffer);  // optional
 *   transport->onEndOfFile = [transport]() mutable { transport->stop(); };
 *   transport->initialize();
 *   transport->start();
 * @endcode
 */
class VASDK_FFMPEG_AUDIO_EXPORT FileTransport : public va::transport::BaseTransport {
public:
    explicit FileTransport(const std::string& filePath);
    ~FileTransport() override;

    //  Transport interface 

    std::string getName()        const override { return "FileTransport"; }
    std::string getDescription() const override {
        return "File audio transport via FFmpeg (" + filePath_ + ")";
    }

    /**
     * @brief Open the file and prepare for streaming.
     *
     * Must be called before start(). Uses the AudioConfig set with
     * setAudioConfig() (default: 16 kHz, mono, 16-bit).
     */
    bool initialize() override;

    /**
     * @brief Start the capture thread and begin emitting AudioFrames.
     */
    bool start() override;

    /**
     * @brief Stop the capture thread.
     *
     * Safe to call from within the onEndOfFile callback (no deadlock).
     */
    void stop() override;

    /**
     * @brief Stop and release all resources, including the audio source.
     */
    void release() override;

    /**
     * @brief No-op for FileTransport (input-only).
     */
    bool sendFrame(std::shared_ptr<va::transport::Frame> frame) override;

    //  Configuration 

    /**
     * @brief Set the output audio format for decoded frames.
     *
     * Must be called before initialize(). Defaults to 16 kHz / mono / SHORT.
     */
    void setAudioConfig(const va::audio::AudioConfig& config) {
        audioConfig_ = config;
    }

    va::audio::AudioConfig getAudioConfig() const { return audioConfig_; }

    /**
     * @brief Number of samples per emitted AudioFrame.
     *
     * Defaults to 2000 (125 ms at 16 kHz). Controls the granularity of
     * frames delivered to the pipeline.
     */
    void   setFrameSize(size_t samples) { _frameSizeInSamples = samples; }
    size_t getFrameSize()         const { return _frameSizeInSamples; }

    /**
     * @brief Attach a shared AudioRingBuffer for decoded sample storage.
     *
     * If set, each AudioFrame's ringbufferOffset / postWriteOffset fields
     * will reference valid positions in this buffer (required by downstream
     * processors such as VADFrameProcessor and TranscriptionProcessor).
     * If not set, those fields are left at zero.
     */
    void setRingBuffer(std::shared_ptr<va::AudioRingBuffer> rb) { _ringBuffer = rb; }
    std::shared_ptr<va::AudioRingBuffer> getRingBuffer() const  { return _ringBuffer; }

    //  Callbacks 

    /**
     * @brief Invoked from the capture thread when the file reaches EOF.
     *
     * Calling stop() or release() from within this callback is safe.
     * It is also safe to call stop() from a different thread concurrently.
     */
    std::function<void()> onEndOfFile;

private:
    void captureThreadFunc();

    std::shared_ptr<va::transport::AudioFrame>
    bufferToFrame(const char* buffer, uint32_t numSamples);

    std::string                       filePath_;
    std::shared_ptr<FFmpegAudioSource> _audioSource;

    va::audio::AudioConfig            audioConfig_;
    size_t                            _frameSizeInSamples{2000};
    uint64_t                          _absoluteOffset{0};
    std::atomic<uint64_t>             _sequenceCounter{0};

    std::thread           _captureThread;
    std::atomic<bool>     _captureRunning{false};

    std::shared_ptr<va::AudioRingBuffer> _ringBuffer;
};
