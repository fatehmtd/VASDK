#include "SileroVAD.h"
#include "SileroVADImpl.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>

SileroVAD::SileroVAD(const std::string& modelPath, SileroVAD::SileroVADConfig sileroVadConfig) :
    _modelPath(modelPath),
    _sileroVadConfig(sileroVadConfig)
{
}

bool SileroVAD::initialize(const va::audio::AudioConfig& audioConfig, int bufferSize)
{
    try
    {
        // Check if model file exists
        std::ifstream file(_modelPath);
        if (!file.good())
        {
            spdlog::error("Cannot find model file: {}", _modelPath);
            return false;
        }
        spdlog::debug("Model file found: {}", _modelPath);
        _vadIterator = std::make_unique<VadIterator>(_modelPath,
            /* Sample_rate = */ audioConfig.sampleRate,
            /* windows_frame_size = */ _sileroVadConfig.windows_frame_size,
            /* Threshold = */ _sileroVadConfig.threshold,
            /* min_silence_duration_ms = */ _sileroVadConfig.min_silence_duration_ms,
            /* speech_pad_ms = */ _sileroVadConfig.speech_pad_ms,
            /* min_speech_duration_ms = */ _sileroVadConfig.min_speech_duration_ms);
        _bufferSize = bufferSize;
        _audioConfig = audioConfig;
    }
    catch (const std::exception& e)
    {
        spdlog::error("Failed to initialize Silero VAD: {}", e.what());
        return false;
    }
    return true;
}

void SileroVAD::release()
{
    _vadIterator.reset();
}

std::vector<va::audio::VADProvider::VADResult> SileroVAD::detectVoiceActivity(const char* buffer, size_t offset, size_t numSamples) const
{
    VASDK_CTRACK_NAME("SileroVAD::detectVoiceActivity");
    if (_vadIterator == nullptr)
    {
        spdlog::error("VAD iterator is not initialized.");
        return {};
    }

    if (numSamples <= 0 || offset < 0 || buffer == nullptr)
    {
        spdlog::error("Invalid offset or length for voice activity detection.");
        return {};
    }

    // Convert the buffer to a vector of floats (local - thread-safe, no shared mutable state)
    const int sampleSize = _audioConfig.getSampleSizeInBytes();
    std::vector<float> audioDataBuffer(_bufferSize, 0.0f);

    std::vector<va::audio::VADProvider::VADResult> vadResults;
    int internalOffset = 0;
    while (internalOffset < numSamples)
    {
        int internalLength = std::min(static_cast<int>(numSamples - internalOffset), static_cast<int>(audioDataBuffer.size()));
        std::fill(audioDataBuffer.begin(), audioDataBuffer.begin() + internalLength, 0.0f);
        switch (_audioConfig.format)
        {
        case va::audio::AudioFormat::FLOAT:
        {
            memcpy(audioDataBuffer.data(), buffer + (offset + internalOffset) * sampleSize, internalLength * sampleSize);
        } break;
        case va::audio::AudioFormat::BYTE: {
            float inv = 1.0f / 128.0f;
            for (int i = 0; i < internalLength; ++i)
            {
                audioDataBuffer[i] = static_cast<float>(static_cast<int8_t>(*(buffer + (offset + internalOffset + i) * sampleSize))) * inv;
            }
        } break;
        case va::audio::AudioFormat::SHORT: {
            float inv = 1.0f / 32768.0f;
            for (int i = 0; i < internalLength; ++i)
            {
                short s;
                memcpy(&s, buffer + (offset + internalOffset + i) * sampleSize, sizeof(short));
                audioDataBuffer[i] = static_cast<float>(s) * inv;
            }
        } break;
        case va::audio::AudioFormat::INT: {
            float inv = 1.0f / 2147483648.0f;
            for (int i = 0; i < internalLength; ++i)
            {
                int v;
                memcpy(&v, buffer + (offset + internalOffset + i) * sampleSize, sizeof(int));
                audioDataBuffer[i] = static_cast<float>(v) * inv;
            }
        } break;
        default:
        spdlog::error("Silero VAD: Unsupported audio format detected.");
        return {};
        }
        _vadIterator->process(audioDataBuffer, internalLength);
        auto speech_timestamps = _vadIterator->get_speech_timestamps();
        vadResults.reserve(vadResults.size() + speech_timestamps.size());

        for (const auto& ts : speech_timestamps)
        {
            va::audio::VADProvider::VADResult result{};
            result.offset = offset + internalOffset + ts.start;
            result.length = ts.end - ts.start;
            result.confidence = ts.probability;
            vadResults.emplace_back(result);
        }

        internalOffset += internalLength;
    }
    return vadResults;
}

void SileroVAD::resetState()
{
    if (_vadIterator)
        _vadIterator->reset();
}

SileroVAD::~SileroVAD()
{
    release();
}