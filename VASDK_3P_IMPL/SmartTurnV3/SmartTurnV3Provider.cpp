#include "SmartTurnV3Provider.h"
#include <utility>
#include "SmartTurnV3Impl.h"
#include <VASDK/Frame.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <limits>

namespace va {
namespace thirdparty::turn {

SmartTurnV3Provider::SmartTurnV3Provider(SmartTurnV3Options options)
    : _threshold(options.threshold),
      _maxAudioDurationSec(options.maxAudioDurationSeconds),
      _numPostAudioFrames(options.postAudioFrames),
      _nFFT(whisper::N_FFT),
      _hopLength(whisper::HOP_LENGTH),
      _nMels(whisper::N_MELS),
      _modelPath(std::move(options.modelPath))
{
    setEvaluateOnPause(options.evaluateOnPause);
    setPauseThreshold(options.pauseThreshold);
}

SmartTurnV3Provider::~SmartTurnV3Provider() = default;

bool SmartTurnV3Provider::initialize(const va::audio::AudioConfig& audioConfig,
    std::shared_ptr<va::AudioRingBuffer> ringBuffer)
{
    if (audioConfig.sampleRate != 16000) {
        spdlog::error("Unsupported sample rate: {}. SmartTurnV3Provider only supports 16kHz audio.", audioConfig.sampleRate);
        return false;
    }
    size_t audioBufferSize = static_cast<size_t>(_maxAudioDurationSec * audioConfig.sampleRate);
    impl_ = std::make_unique<SmartTurnV3Impl>(_modelPath, _nFFT, _hopLength, _nMels, audioBufferSize);
    if (!impl_->initialize(audioConfig)) {
        spdlog::error("Failed to initialize SmartTurnV3Impl within SmartTurnV3Provider");
        return false;
    }
    _audioConfig = audioConfig;
    _audioRingBuffer = ringBuffer;
    _audioBuffer.reserve(audioBufferSize);
    _audioBuffer.resize(_audioBuffer.capacity(), 0.0f);
    // Pre-allocate raw read buffer (worst-case: maxDuration samples × num bytes per sample)
    _audioScratchBytes.resize(_audioBuffer.size() * _audioConfig.getSampleSizeInBytes());
    return true;
}

va::audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::evaluate(
    const std::shared_ptr<va::transport::Frame> frame,
    bool agentIsSpeaking)
{
    VASDK_CTRACK_NAME("SmartTurnV3::evaluate");
    if (frame->getType() == va::transport::FrameType::VAD_STATE) {
        auto vadFrame = std::static_pointer_cast<va::transport::VADStateFrame>(frame);
        return handleVADStateFrame(vadFrame, agentIsSpeaking);
    }
    if (frame->getType() == va::transport::FrameType::AUDIO) {
        auto audioFrame = std::static_pointer_cast<va::transport::AudioFrame>(frame);
        return handleAudioFrame(audioFrame);
    }
    return {};
}

va::audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::handleVADStateFrame(
    const std::shared_ptr<va::transport::VADStateFrame>& frame,
    bool agentIsSpeaking)
{
    _paused = frame->state == va::transport::VADState::PAUSED || frame->state == va::transport::VADState::ENDING;
    if (frame->state == va::transport::VADState::STARTING) {
        _vadActive = true;
        _vadStartOffset = frame->speechStartOffset;
        _postAudioFramesProcessed = 0;
        _vadOccured = true;

        auto type = agentIsSpeaking
            ? va::transport::TurnStateFrame::TurnType::INTERRUPT
            : va::transport::TurnStateFrame::TurnType::NORMAL;

        return { Decision::TURN_START, frame->confidence,
                 va::transport::TurnStateFrame::TurnActor::USER, type };
    } else if (frame->state == va::transport::VADState::ENDED) {
        _vadActive = false;
        _vadEndOffset = frame->speechEndOffset;
    }

    return {};
}

va::audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::handleAudioFrame(
    const std::shared_ptr<va::transport::AudioFrame>& frame)
{
    // Run inference in the post-VAD window (after ENDED, before limit), or during a pause if enabled.
    if (!_vadOccured) return {};
    const bool inPause = _vadActive && _evaluateOnPause && _paused;
    if (_vadActive && !inPause) return {};
    if (!inPause && _postAudioFramesProcessed >= _numPostAudioFrames) return {};

    std::fill(_audioBuffer.begin(), _audioBuffer.end(), 0.0f);

    // Read from the VAD start offset up to the current frame's offset.
    long audioDuration = _audioRingBuffer->computeDeltaOffset(_vadStartOffset, frame->ringbufferOffset);
    long maxDurationInSamples = std::min(audioDuration, static_cast<long>(_audioBuffer.size()));
    // Use pre-allocated scratch buffer - resize only if somehow larger than worst-case (shouldn't happen)
    size_t bytesNeeded = static_cast<size_t>(maxDurationInSamples) * _audioConfig.getSampleSizeInBytes();
    if (_audioScratchBytes.size() < bytesNeeded)
        _audioScratchBytes.resize(bytesNeeded);
    char* audioData = _audioScratchBytes.data();

    long deltaOffset = audioDuration - _audioBuffer.size();
    if(deltaOffset < 0) deltaOffset = 0;

    int numRead = _audioRingBuffer->readAbsolute(_vadStartOffset + deltaOffset, audioData, 0, maxDurationInSamples);

    if (numRead < 0) {
        spdlog::warn("[SmartTurnV3Provider] Failed to read audio data from ring buffer");
        return {};
    }

    convertAudioToFloat(audioData, 0, _audioBuffer.data(), 0, numRead);

    float turnProbability = impl_->processAudio(_audioBuffer.data(), 0, numRead);
    if (!inPause) ++_postAudioFramesProcessed;

    spdlog::debug("[SmartTurnV3Provider] Audio frame: {} samples, turn probability: {:.2f}",
        numRead, turnProbability);

    // A pause is often just a breath: ending the turn there takes more confidence.
    if (turnProbability >= (inPause ? _pauseThreshold : _threshold)) {
        spdlog::info("[SmartTurnV3Provider] Turn end detected{} (probability: {:.2f})",
            inPause ? " during pause" : "", turnProbability);
        _postAudioFramesProcessed = 0;
        _vadOccured = false;
        _paused = false;
        return { Decision::TURN_END, turnProbability,
                 va::transport::TurnStateFrame::TurnActor::USER,
                 va::transport::TurnStateFrame::TurnType::NORMAL };
    }

    return {};
}

void SmartTurnV3Provider::convertAudioToFloat(const char* audioData,
    size_t srcOffset,
    float* floatBuffer,
    size_t dstOffset,
    int numSamples)
{
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::BYTE: {
        float mul = 1.0f / static_cast<float>(std::numeric_limits<int8_t>::max());
        for (int i = 0; i < numSamples; ++i) {
            floatBuffer[dstOffset + i] = static_cast<float>(reinterpret_cast<const int8_t*>(audioData + srcOffset)[i]) * mul;
        }
        break;
    }
    case va::audio::AudioFormat::SHORT: {
        float mul = 1.0f / static_cast<float>(std::numeric_limits<int16_t>::max());
        for (int i = 0; i < numSamples; ++i) {
            floatBuffer[dstOffset + i] = static_cast<float>(reinterpret_cast<const int16_t*>(audioData + srcOffset)[i]) * mul;
        }
        break;
    }
    case va::audio::AudioFormat::INT: {
        float mul = 1.0f / static_cast<float>(std::numeric_limits<int32_t>::max());
        for (int i = 0; i < numSamples; ++i) {
            floatBuffer[dstOffset + i] = static_cast<float>(reinterpret_cast<const int32_t*>(audioData + srcOffset)[i]) * mul;
        }
        break;
    }
    case va::audio::AudioFormat::FLOAT:
        std::memcpy(floatBuffer + dstOffset, audioData + srcOffset, numSamples * sizeof(float));
        break;
    default:
        spdlog::warn("[SmartTurnV3Provider] Unsupported audio format: {}", static_cast<int>(_audioConfig.format));
        break;
    }
}

} // namespace thirdparty::turn
} // namespace va
