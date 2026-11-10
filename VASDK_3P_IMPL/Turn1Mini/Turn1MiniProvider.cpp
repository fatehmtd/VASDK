#include "Turn1MiniProvider.h"

#include <cstring>
#include <limits>
#include <utility>

#include <VASDK/Frame.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include "Turn1MiniStream.h"

namespace va::thirdparty::turn {

namespace {

constexpr int kSampleRate = 16000;

// Interleaved samples of the configured format to float in [-1, 1]
bool toFloat(va::audio::AudioFormat format, const char* bytes, float* samples, int count) {
    switch (format) {
    case va::audio::AudioFormat::BYTE: {
        const float scale = 1.0f / static_cast<float>(std::numeric_limits<int8_t>::max());
        for (int i = 0; i < count; ++i) samples[i] = static_cast<float>(reinterpret_cast<const int8_t*>(bytes)[i]) * scale;
        return true;
    }
    case va::audio::AudioFormat::SHORT: {
        const float scale = 1.0f / static_cast<float>(std::numeric_limits<int16_t>::max());
        for (int i = 0; i < count; ++i) samples[i] = static_cast<float>(reinterpret_cast<const int16_t*>(bytes)[i]) * scale;
        return true;
    }
    case va::audio::AudioFormat::INT: {
        const float scale = 1.0f / static_cast<float>(std::numeric_limits<int32_t>::max());
        for (int i = 0; i < count; ++i) samples[i] = static_cast<float>(reinterpret_cast<const int32_t*>(bytes)[i]) * scale;
        return true;
    }
    case va::audio::AudioFormat::FLOAT:
        std::memcpy(samples, bytes, static_cast<size_t>(count) * sizeof(float));
        return true;
    default:
        return false;
    }
}

} // namespace

Turn1MiniProvider::Turn1MiniProvider(Turn1MiniOptions options) : _options(std::move(options)) {
    setEvaluateOnPause(_options.evaluateOnPause);
    setPauseThreshold(_options.pauseThreshold);
}

Turn1MiniProvider::~Turn1MiniProvider() = default;

bool Turn1MiniProvider::initialize(const va::audio::AudioConfig& audioConfig, std::shared_ptr<va::AudioRingBuffer> ringBuffer) {
    if (audioConfig.sampleRate != kSampleRate || audioConfig.channels != 1) {
        spdlog::error("[Turn1Mini] needs 16 kHz mono audio, got {} Hz, {} channel(s)", audioConfig.sampleRate, audioConfig.channels);
        return false;
    }
    if (!ringBuffer) {
        spdlog::error("[Turn1Mini] needs the session's audio ring buffer");
        return false;
    }
    auto stream = std::make_unique<Turn1MiniStream>(_options.modelPath);
    if (!stream->initialize()) return false;
    _stream = std::move(stream);
    _audioConfig = audioConfig;
    _ringBuffer = std::move(ringBuffer);
    spdlog::info("[Turn1Mini] loaded {} (threshold {:.2f}, pause threshold {:.2f})", _options.modelPath, _options.threshold,
                 pauseThreshold());
    return true;
}

void Turn1MiniProvider::reset() {
    if (_stream) _stream->reset();
    _vadActive = _vadOccurred = _paused = false;
    _postAudioFramesProcessed = 0;
}

va::audio::TurnDetectionProvider::TurnEvaluation Turn1MiniProvider::evaluate(const std::shared_ptr<va::transport::Frame> frame,
                                                                            bool agentIsSpeaking) {
    VASDK_CTRACK_NAME("Turn1Mini::evaluate");
    if (!frame || !_stream) return {};
    if (frame->getType() == va::transport::FrameType::VAD_STATE)
        return handleVadState(static_cast<const va::transport::VADStateFrame&>(*frame), agentIsSpeaking);
    if (frame->getType() == va::transport::FrameType::AUDIO)
        return handleAudio(static_cast<const va::transport::AudioFrame&>(*frame));
    return {};
}

va::audio::TurnDetectionProvider::TurnEvaluation Turn1MiniProvider::handleVadState(const va::transport::VADStateFrame& frame,
                                                                                  bool agentIsSpeaking) {
    _paused = frame.state == va::transport::VADState::PAUSED || frame.state == va::transport::VADState::ENDING;
    if (frame.state == va::transport::VADState::STARTING) {
        _vadActive = true;
        _vadOccurred = true;
        _postAudioFramesProcessed = 0;
        const auto type = agentIsSpeaking ? va::transport::TurnStateFrame::TurnType::INTERRUPT
                                          : va::transport::TurnStateFrame::TurnType::NORMAL;
        return {Decision::TURN_START, frame.confidence, va::transport::TurnStateFrame::TurnActor::USER, type};
    }
    if (frame.state == va::transport::VADState::ENDED) _vadActive = false;
    return {};
}

bool Turn1MiniProvider::hear(const va::transport::AudioFrame& frame) {
    const auto count = static_cast<int>(frame.numSamples);
    if (count <= 0) return false;
    const size_t bytes = static_cast<size_t>(count) * static_cast<size_t>(_audioConfig.getSampleSizeInBytes());
    if (_frameBytes.size() < bytes) _frameBytes.resize(bytes);
    if (_frameSamples.size() < static_cast<size_t>(count)) _frameSamples.resize(static_cast<size_t>(count));
    const int read = _ringBuffer->readAbsolute(static_cast<long>(frame.ringbufferOffset), _frameBytes.data(), 0, count);
    if (read <= 0) {
        spdlog::warn("[Turn1Mini] can't read the frame's audio from the ring buffer");
        return false;
    }
    if (!toFloat(_audioConfig.format, _frameBytes.data(), _frameSamples.data(), read)) {
        spdlog::warn("[Turn1Mini] unsupported audio format {}", static_cast<int>(_audioConfig.format));
        return false;
    }
    _stream->push(_frameSamples.data(), static_cast<size_t>(read));
    return true;
}

va::audio::TurnDetectionProvider::TurnEvaluation Turn1MiniProvider::handleAudio(const va::transport::AudioFrame& frame) {
    // The stream hears every frame, so the model has its context when a pause comes
    if (!hear(frame)) return {};

    // Decide in a pause (if enabled), or in the few frames after the VAD's end
    if (!_vadOccurred) return {};
    const bool inPause = _vadActive && _evaluateOnPause && _paused;
    if (_vadActive && !inPause) return {};
    if (!inPause && _postAudioFramesProcessed >= _options.postAudioFrames) return {};

    const float score = _stream->scoreNow();
    if (!inPause) ++_postAudioFramesProcessed;
    spdlog::debug("[Turn1Mini] end-of-turn score {:.3f}{}", score, inPause ? " (pause)" : "");

    // A pause is often just a breath: ending the turn there can take more confidence
    if (score >= (inPause ? _pauseThreshold : _options.threshold)) {
        spdlog::info("[Turn1Mini] turn end{} (score {:.2f})", inPause ? " during a pause" : "", score);
        _postAudioFramesProcessed = 0;
        _vadOccurred = false;
        _paused = false;
        return {Decision::TURN_END, score, va::transport::TurnStateFrame::TurnActor::USER,
                va::transport::TurnStateFrame::TurnType::NORMAL};
    }
    return {};
}

} // namespace va::thirdparty::turn
