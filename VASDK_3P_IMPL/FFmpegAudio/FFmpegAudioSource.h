#pragma once

#include <VASDK/GenericAudioSource.h>
#include "vasdk_ffmpeg_audio_export.h"

#include <atomic>
#include <string>
#include <vector>

// Forward-declare FFmpeg C types to keep them out of this public header.
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

/**
 * @brief Audio source that reads any file format supported by FFmpeg.
 *
 * FFmpegAudioSource implements va::audio::GenericAudioSource by using FFmpeg
 * to open and decode audio containers and codecs (MP3, FLAC, WAV, OGG, M4A,
 * OPUS, AAC, WMA, …) and resamples the decoded PCM to whatever AudioConfig
 * the caller requests (sample rate, channel count, and sample format).
 *
 * Typical usage via FileTransport:
 * @code
 *   auto transport = std::make_shared<FileTransport>("/path/to/clip.flac");
 *   transport->setAudioConfig({16000, va::audio::AudioFormat::SHORT, 1});
 *   transport->onEndOfFile = [&]{ transport->stop(); };
 *   transport->initialize();
 *   transport->start();
 * @endcode
 *
 * Direct usage (e.g., with LocalTransport + a null sink):
 * @code
 *   auto src = std::make_shared<FFmpegAudioSource>("/path/to/clip.mp3");
 *   va::audio::AudioConfig cfg{16000, va::audio::AudioFormat::SHORT, 1};
 *   src->initializeAudioSource(cfg);
 *   src->startAudioSource();
 *   // ... loop: src->readAudio(buf, 0, 2000) until 0 is returned (EOF) ...
 *   src->stopAudioSource();
 *   src->releaseAudioSource();
 * @endcode
 *
 * Thread safety: readAudio() must be called from a single thread.
 *
 * Requires FFmpeg >= 5.1 (libswresample >= 4.0) for the AVChannelLayout API.
 */
class VASDK_FFMPEG_AUDIO_EXPORT FFmpegAudioSource : public va::audio::GenericAudioSource {
public:
    explicit FFmpegAudioSource(const std::string& filePath);
    ~FFmpegAudioSource() override;

    //  GenericAudioSource interface 

    bool isAudioSourceReady()   const override { return ready_; }
    bool isAudioSourceStarted() const override { return started_; }

    /**
     * @brief Open the file, select the best audio stream, open the decoder,
     *        and set up a SwrContext to resample to @p config.
     *
     * @param config  Target sample rate / format / channel count.
     * @return true on success.
     */
    bool initializeAudioSource(const va::audio::AudioConfig& config) override;

    /**
     * @brief Release all FFmpeg resources.
     *
     * Safe to call even if initialization failed. After calling this,
     * initializeAudioSource() may be called again.
     */
    void releaseAudioSource() override;

    /**
     * @brief Read up to @p length decoded, resampled samples into @p buffer.
     *
     * @param buffer  Destination buffer.
     * @param offset  Start offset within @p buffer, in samples (not bytes).
     * @param length  Number of samples requested.
     * @return Number of samples actually written (>0),
     *         0 at end-of-file,
     *        -1 on error or if not initialised/started.
     */
    int readAudio(char* buffer, int offset, int length) override;

    size_t getAvailableAudioSamples() const override;

    bool startAudioSource() override;
    void stopAudioSource()  override;

    /** @brief Returns true once the end of the input file has been reached. */
    bool isEOF() const { return eof_.load(); }

private:
    // Decode the next available audio frame and push it through the resampler
    // into stagingBuf_. Returns false when no more data is available.
    bool fillStagingBuffer();

    // Resample the current AVFrame into stagingBuf_.
    bool resampleIntoStaging();

    // Copy up to @p count samples from stagingBuf_ into @p dst (packed format).
    // Returns the number of samples copied.
    int drainStaging(char* dst, int count);

    // Samples currently sitting in stagingBuf_ that have not yet been served.
    int availableSamples() const;

    // Release all FFmpeg handles without touching ready_ / started_ flags.
    void freeFFmpegResources();

    //  FFmpeg state 
    std::string      filePath_;
    AVFormatContext* formatCtx_       = nullptr;
    AVCodecContext*  codecCtx_        = nullptr;
    AVPacket*        packet_          = nullptr;
    AVFrame*         frame_           = nullptr;
    SwrContext*      swrCtx_          = nullptr;
    int              audioStreamIndex_ = -1;
    bool             decoderFlushed_  = false; // true after null flush-packet sent

    //  Lifecycle flags 
    bool              ready_   = false;
    bool              started_ = false;
    std::atomic<bool> eof_{false};

    //  Staging buffer 
    // Holds decoded + resampled samples in the output format.
    // stagingReadIdx_ is the byte offset of the next sample to serve.
    std::vector<uint8_t> stagingBuf_;
    size_t               stagingReadIdx_ = 0;

    // The config the caller passed to initializeAudioSource().
    va::audio::AudioConfig requestedConfig_;
};
