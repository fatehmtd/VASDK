#include "SmartTurnV3Provider.h"
#include "SmartTurnV3Impl.h"
#include <VASDK/Frame.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <limits>

namespace va {
namespace thirdparty {

SmartTurnV3Provider::SmartTurnV3Provider(const std::string& modelPath,
    float threshold,
    float maxAudioDurationSec,
    int numPostAudioFrames,
    int nFFT,
    int hopLength,
    int nMels) : _modelPath(modelPath),
                 _nFFT(nFFT),
                 _hopLength(hopLength),
                 _nMels(nMels),
                 _maxAudioDurationSec(maxAudioDurationSec),
                 _numPostAudioFrames(numPostAudioFrames),
                 _threshold(threshold)
{

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

audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::evaluate(
    const std::shared_ptr<transport::Frame> frame,
    bool agentIsSpeaking)
{
    VASDK_CTRACK_NAME("SmartTurnV3::evaluate");
    if (frame->getType() == transport::FrameType::VAD_STATE) {
        auto vadFrame = std::static_pointer_cast<transport::VADStateFrame>(frame);
        return handleVADStateFrame(vadFrame, agentIsSpeaking);
    }
    if (frame->getType() == transport::FrameType::AUDIO) {
        auto audioFrame = std::static_pointer_cast<transport::AudioFrame>(frame);
        return handleAudioFrame(audioFrame);
    }
    return {};
}

audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::handleVADStateFrame(
    const std::shared_ptr<transport::VADStateFrame>& frame,
    bool agentIsSpeaking)
{
    if (frame->state == transport::VADState::STARTING) {
        _vadActive = true;
        _vadStartOffset = frame->speechStartOffset;
        _postAudioFramesProcessed = 0;
        _vadOccured = true;

        auto type = agentIsSpeaking
            ? transport::TurnStateFrame::TurnType::INTERRUPT
            : transport::TurnStateFrame::TurnType::NORMAL;

        return { Decision::TURN_START, frame->confidence,
                 transport::TurnStateFrame::TurnActor::USER, type };
    } else if (frame->state == transport::VADState::ENDED) {
        _vadActive = false;
        _vadEndOffset = frame->speechEndOffset;
    }

    return {};
}

audio::TurnDetectionProvider::TurnEvaluation SmartTurnV3Provider::handleAudioFrame(
    const std::shared_ptr<transport::AudioFrame>& frame)
{
    // Only run inference in the post-VAD window (after ENDED, before limit).
    if (_vadActive || !_vadOccured) return {};
    if (_postAudioFramesProcessed >= _numPostAudioFrames) return {};

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
    ++_postAudioFramesProcessed;

    spdlog::debug("[SmartTurnV3Provider] Audio frame: {} samples, turn probability: {:.2f}",
        numRead, turnProbability);

    if (turnProbability >= _threshold) {
        spdlog::info("[SmartTurnV3Provider] Turn end detected (probability: {:.2f})", turnProbability);
        _postAudioFramesProcessed = 0;
        _vadOccured = false;
        return { Decision::TURN_END, turnProbability,
                 transport::TurnStateFrame::TurnActor::USER,
                 transport::TurnStateFrame::TurnType::NORMAL };
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

} // namespace thirdparty
} // namespace va
