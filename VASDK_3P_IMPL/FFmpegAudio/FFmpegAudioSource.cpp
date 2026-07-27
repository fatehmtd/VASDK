#include "FFmpegAudioSource.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

//  Helpers 

namespace {

/// Map a VASDK AudioFormat to the equivalent packed (non-planar) AVSampleFormat.
AVSampleFormat toAVSampleFormat(va::audio::AudioFormat fmt) {
    switch (fmt) {
        case va::audio::AudioFormat::BYTE:  return AV_SAMPLE_FMT_U8;
        case va::audio::AudioFormat::SHORT: return AV_SAMPLE_FMT_S16;
        case va::audio::AudioFormat::INT:   return AV_SAMPLE_FMT_S32;
        case va::audio::AudioFormat::FLOAT: return AV_SAMPLE_FMT_FLT;
        default:                            return AV_SAMPLE_FMT_S16;
    }
}

} // anonymous namespace

//  Lifecycle 

FFmpegAudioSource::FFmpegAudioSource(const std::string& filePath)
    : filePath_(filePath)
{}

FFmpegAudioSource::~FFmpegAudioSource() {
    stopAudioSource();
    releaseAudioSource();
}

bool FFmpegAudioSource::initializeAudioSource(const va::audio::AudioConfig& config) {
    if (ready_) {
        spdlog::warn("[FFmpegAudioSource] Already initialized");
        return true;
    }

    requestedConfig_ = config;
    _audioConfig     = config;

    //  Open container 
    if (avformat_open_input(&formatCtx_, filePath_.c_str(), nullptr, nullptr) != 0) {
        spdlog::error("[FFmpegAudioSource] Cannot open file: {}", filePath_);
        return false;
    }

    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        spdlog::error("[FFmpegAudioSource] Cannot find stream info in: {}", filePath_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    //  Select best audio stream 
    const AVCodec* codec = nullptr;
    audioStreamIndex_ = av_find_best_stream(
        formatCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (audioStreamIndex_ < 0 || !codec) {
        spdlog::error("[FFmpegAudioSource] No audio stream found in: {}", filePath_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    //  Open decoder 
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        spdlog::error("[FFmpegAudioSource] Cannot allocate codec context");
        avformat_close_input(&formatCtx_);
        return false;
    }

    AVStream* stream = formatCtx_->streams[audioStreamIndex_];
    if (avcodec_parameters_to_context(codecCtx_, stream->codecpar) < 0) {
        spdlog::error("[FFmpegAudioSource] Cannot copy codec parameters");
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        spdlog::error("[FFmpegAudioSource] Cannot open codec '{}'", codec->name);
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    spdlog::info("[FFmpegAudioSource] Stream opened: codec={} src={}Hz/{}ch/{}",
        codec->name,
        codecCtx_->sample_rate,
        codecCtx_->ch_layout.nb_channels,
        av_get_sample_fmt_name(codecCtx_->sample_fmt));

    //  Set up resampler 
    // Use av_channel_layout_copy so that AV_CHANNEL_ORDER_CUSTOM layouts
    // (which carry a heap-allocated array) are properly reference-counted.
    AVChannelLayout srcLayout{};
    av_channel_layout_copy(&srcLayout, &codecCtx_->ch_layout);
    if (srcLayout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_uninit(&srcLayout);
        av_channel_layout_default(&srcLayout, codecCtx_->ch_layout.nb_channels);
    }

    AVChannelLayout dstLayout{};
    av_channel_layout_default(&dstLayout, config.channels);

    AVSampleFormat outFmt = toAVSampleFormat(config.format);

    int ret = swr_alloc_set_opts2(
        &swrCtx_,
        &dstLayout,            outFmt,               config.sampleRate,  // output
        &srcLayout,            codecCtx_->sample_fmt, codecCtx_->sample_rate, // input
        0, nullptr);

    av_channel_layout_uninit(&dstLayout);
    av_channel_layout_uninit(&srcLayout);

    if (ret < 0 || !swrCtx_) {
        spdlog::error("[FFmpegAudioSource] swr_alloc_set_opts2 failed ({})", ret);
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    if (swr_init(swrCtx_) < 0) {
        spdlog::error("[FFmpegAudioSource] swr_init failed");
        swr_free(&swrCtx_);
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    //  Allocate packet/frame handles 
    packet_ = av_packet_alloc();
    frame_  = av_frame_alloc();
    if (!packet_ || !frame_) {
        spdlog::error("[FFmpegAudioSource] Cannot allocate packet/frame");
        freeFFmpegResources();
        return false;
    }

    ready_ = true;
    spdlog::info("[FFmpegAudioSource] Initialized: {} → {}Hz/{}ch ({}bps/sample)",
        filePath_,
        config.sampleRate, config.channels,
        config.getSampleSizeInBytes() * 8 / std::max(config.channels, 1));
    return true;
}

void FFmpegAudioSource::releaseAudioSource() {
    if (!ready_) return;
    freeFFmpegResources();
    stagingBuf_.clear();
    stagingReadIdx_ = 0;
    decoderFlushed_ = false;
    eof_            = false;
    ready_          = false;
    spdlog::info("[FFmpegAudioSource] Released");
}

void FFmpegAudioSource::freeFFmpegResources() {
    if (packet_)    { av_packet_free(&packet_);        packet_    = nullptr; }
    if (frame_)     { av_frame_free(&frame_);          frame_     = nullptr; }
    if (swrCtx_)    { swr_free(&swrCtx_);              swrCtx_    = nullptr; }
    if (codecCtx_)  { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (formatCtx_) { avformat_close_input(&formatCtx_); formatCtx_ = nullptr; }
    audioStreamIndex_ = -1;
}

bool FFmpegAudioSource::startAudioSource() {
    if (!ready_) {
        spdlog::error("[FFmpegAudioSource] Cannot start: not initialized");
        return false;
    }
    started_ = true;
    return true;
}

void FFmpegAudioSource::stopAudioSource() {
    started_ = false;
}

//  readAudio 

int FFmpegAudioSource::readAudio(char* buffer, int offset, int length) {
    if (!ready_ || !started_) return -1;

    const int bytesPerSample = requestedConfig_.getSampleSizeInBytes();
    char* dst        = buffer + static_cast<ptrdiff_t>(offset) * bytesPerSample;
    int   totalRead  = 0;

    while (totalRead < length) {
        // Serve whatever is already in the staging buffer.
        const int staged = availableSamples();
        if (staged > 0) {
            const int toDrain = std::min(staged, length - totalRead);
            drainStaging(dst + static_cast<ptrdiff_t>(totalRead) * bytesPerSample, toDrain);
            totalRead += toDrain;
            continue;
        }

        // Staging buffer is empty – try to decode more.
        if (eof_.load()) break;

        if (!fillStagingBuffer()) {
            eof_ = true;
            break;
        }
    }

    // Returns 0 at EOF (nothing was read and nothing left in buffer).
    return totalRead;
}

size_t FFmpegAudioSource::getAvailableAudioSamples() const
{
    return availableSamples();
}

//  Private helpers 

int FFmpegAudioSource::availableSamples() const {
    const size_t availBytes    = stagingBuf_.size() - stagingReadIdx_;
    const int    bytesPerSample = requestedConfig_.getSampleSizeInBytes();
    return (bytesPerSample > 0)
        ? static_cast<int>(availBytes / static_cast<size_t>(bytesPerSample))
        : 0;
}

int FFmpegAudioSource::drainStaging(char* dst, int count) {
    const int    bytesPerSample = requestedConfig_.getSampleSizeInBytes();
    const size_t toCopy         = static_cast<size_t>(count) * bytesPerSample;
    std::memcpy(dst, stagingBuf_.data() + stagingReadIdx_, toCopy);
    stagingReadIdx_ += toCopy;

    // Compact the staging buffer periodically to prevent unbounded growth.
    // Compact when more than 64 KiB has been consumed and we've consumed at
    // least half the buffer (avoids O(n) copies on every small read).
    constexpr size_t kCompactThreshold = 65536;
    if (stagingReadIdx_ > kCompactThreshold &&
        stagingReadIdx_ >= stagingBuf_.size() / 2) {
        stagingBuf_.erase(
            stagingBuf_.begin(),
            stagingBuf_.begin() + static_cast<ptrdiff_t>(stagingReadIdx_));
        stagingReadIdx_ = 0;
    }

    return count;
}

bool FFmpegAudioSource::fillStagingBuffer() {
    // This function pumps the demux→decode→resample pipeline one step at a
    // time until at least one audio frame lands in stagingBuf_.
    //
    // State machine:
    //   avcodec_receive_frame → EAGAIN  → feed next packet → loop
    //                        → 0       → resample into staging, return true
    //                        → EOF     → return false
    //                        → error   → return false
    while (true) {
        const int ret = avcodec_receive_frame(codecCtx_, frame_);

        if (ret == 0) {
            // Got a decoded audio frame.
            return resampleIntoStaging();
        }

        if (ret == AVERROR_EOF) {
            // Decoder has been fully drained.
            return false;
        }

        if (ret == AVERROR(EAGAIN)) {
            // Decoder needs another packet.

            if (decoderFlushed_) {
                // We already sent the null flush-packet; nothing more to read.
                return false;
            }

            // Read the next demuxed packet.
            const int readRet = av_read_frame(formatCtx_, packet_);
            if (readRet == AVERROR_EOF) {
                // Container exhausted: flush buffered frames from the decoder.
                avcodec_send_packet(codecCtx_, nullptr);
                decoderFlushed_ = true;
                continue; // loop to drain decoder
            }
            if (readRet < 0) {
                spdlog::error("[FFmpegAudioSource] av_read_frame error: {}", readRet);
                return false;
            }

            // Forward only the audio packets for the selected stream.
            if (packet_->stream_index == audioStreamIndex_) {
                const int sendRet = avcodec_send_packet(codecCtx_, packet_);
                if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                    spdlog::error("[FFmpegAudioSource] avcodec_send_packet error: {}", sendRet);
                    av_packet_unref(packet_);
                    return false;
                }
            }
            av_packet_unref(packet_);
            continue; // loop to avcodec_receive_frame
        }

        // Unexpected codec error.
        spdlog::error("[FFmpegAudioSource] avcodec_receive_frame error: {}", ret);
        return false;
    }
}

bool FFmpegAudioSource::resampleIntoStaging() {
    // Calculate a safe upper bound for the number of output samples.
    // swr_get_delay accounts for samples already buffered inside the resampler.
    const int64_t delay = swr_get_delay(swrCtx_, codecCtx_->sample_rate);
    const int outSamples = static_cast<int>(
        av_rescale_rnd(delay + frame_->nb_samples,
                       requestedConfig_.sampleRate,
                       codecCtx_->sample_rate,
                       AV_ROUND_UP));

    const int    bytesPerSample = requestedConfig_.getSampleSizeInBytes();
    const size_t neededBytes    = static_cast<size_t>(outSamples) * bytesPerSample;

    // Reserve space at the tail of the staging buffer.
    const size_t writeStart = stagingBuf_.size();
    stagingBuf_.resize(writeStart + neededBytes);
    uint8_t* outPtr = stagingBuf_.data() + writeStart;

    // swr_convert writes interleaved (packed) samples into outPtr.
    // frame_->data[] carries the decoded samples (planar or packed).
    const int frameNbSamples  = frame_->nb_samples; // capture before unref
    const int samplesConverted = swr_convert(
        swrCtx_,
        &outPtr, outSamples,
        (const uint8_t**)frame_->data, frameNbSamples); // NOLINT(*-cast*)

    // Release the decoded frame back to FFmpeg. This MUST be called after every
    // successful avcodec_receive_frame() so the decoder can recycle its buffers.
    // Skipping this causes the decoder to stall after the first frame.
    av_frame_unref(frame_);

    if (samplesConverted < 0) {
        spdlog::error("[FFmpegAudioSource] swr_convert failed: {}", samplesConverted);
        stagingBuf_.resize(writeStart); // roll back
        return false;
    }

    spdlog::trace("[FFmpegAudioSource] swr_convert: {} → {} samples",
        frameNbSamples, samplesConverted);

    // Trim to the number of samples actually produced.
    stagingBuf_.resize(writeStart + static_cast<size_t>(samplesConverted) * bytesPerSample);
    return true;
}
