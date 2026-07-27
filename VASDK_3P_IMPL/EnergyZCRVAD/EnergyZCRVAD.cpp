#include "EnergyZCRVAD.h"

#include <VASDK/AudioSIMD.h>
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

EnergyZCRVAD::EnergyZCRVAD(int frameSizeMs, double energyThreshold, double zcrThreshold)
    : frameSizeMs_(frameSizeMs),
      _energyThreshold(energyThreshold),
       zcrThreshold_(zcrThreshold)
{
}

std::vector<va::audio::VADProvider::VADResult> EnergyZCRVAD::detectVoiceActivity(const char *buffer, size_t offset, size_t length) const
{
    if (buffer == nullptr || length == 0)
    {
        spdlog::error("Error: Invalid audio buffer, offset, or frame size.");
        return {};
    }

    const int sampleSize = _audioConfig.getSampleSizeInBytes();
    std::vector<float> audioBuffer(length, 0.0f);

    switch(_audioConfig.format)
    {
        case va::audio::AudioFormat::SHORT: {
                const auto* src = reinterpret_cast<const int16_t*>(buffer + offset * sampleSize);
                AudioSIMD::convertInt16ToFloat(src, audioBuffer.data(), length);
            }
            break;
        case va::audio::AudioFormat::BYTE: {
                const auto* src = reinterpret_cast<const uint8_t*>(buffer + offset * sampleSize);
                AudioSIMD::convertByteToFloat(src, audioBuffer.data(), length);
            }
            break;
        case va::audio::AudioFormat::INT: {
                const auto* src = reinterpret_cast<const int32_t*>(buffer + offset * sampleSize);
                AudioSIMD::convertInt32ToFloat(src, audioBuffer.data(), length);
            }
            break;
        case va::audio::AudioFormat::FLOAT: {
                std::memcpy(audioBuffer.data(),
                            buffer + offset * sampleSize,
                            length * sizeof(float));
            }
            break;
        default:
            spdlog::error("EnergyZCRVAD: Unsupported audio format.");
            return {};
    }

    for (size_t i = 0; i < audioBuffer.size(); i += frameSizeSamples_)
    {
        size_t currentFrameEnd = std::min(i + frameSizeSamples_, audioBuffer.size());
        std::vector<float> currentFrame(audioBuffer.begin() + i, audioBuffer.begin() + currentFrameEnd);

        // Zero-pad the final partial frame to a full frame size rather than discarding it,
        // so speech at the tail of a buffer is not missed.
        if (currentFrame.size() < frameSizeSamples_)
        {
            currentFrame.resize(frameSizeSamples_, 0.0f);
        }
        if (currentFrame.empty())
        {
            continue;
        }

        double frameEnergy = calculateEnergy(currentFrame);
        double frameZCR = calculateZeroCrossingRate(currentFrame);

        // Voice if energy is above threshold and ZCR stays below the upper bound,
        // which rejects high-frequency noise that would otherwise pass on energy alone.
        if (frameEnergy > _energyThreshold)
        {
            if (frameZCR < zcrThreshold_)
            {
                return {
                    {i, currentFrame.size()}
                };
            }
        }
    }
    return {};
}

double EnergyZCRVAD::calculateEnergy(const std::vector<float> &frame) const
{
    return static_cast<double>(AudioSIMD::computeEnergy(frame.data(), frame.size()));
}

double EnergyZCRVAD::calculateZeroCrossingRate(const std::vector<float> &frame) const
{
    if (frame.size() <= 1) return 0.0;
    return static_cast<double>(AudioSIMD::computeZCR(frame.data(), frame.size()))
           / static_cast<double>(frame.size() - 1);
}

void EnergyZCRVAD::release()
{
}

bool EnergyZCRVAD::initialize(const va::audio::AudioConfig &audioConfig, int bufferSize)
{
    _audioConfig = audioConfig;
    frameSizeSamples_ = static_cast<int>((frameSizeMs_ * audioConfig.sampleRate) / 1000.0);
    spdlog::info("Frame size in samples: {}", frameSizeSamples_);
    if (frameSizeSamples_ <= 0)
    {
        std::cerr << "Error: Calculated frame size in samples is zero or negative. Adjust frameSizeMs or sampleRate." << std::endl;
        frameSizeSamples_ = 1; // Prevent division by zero
    }
    return true;
}
