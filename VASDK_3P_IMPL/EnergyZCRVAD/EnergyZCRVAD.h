#pragma once

#include <vector>
#include <numeric>
#include <cmath>
#include <iostream>
#include <algorithm> // For std::abs and std::min
#include <VASDK/VADProvider.h>

class EnergyZCRVAD: public va::audio::VADProvider  {
public:
    EnergyZCRVAD(int frameSizeMs, double energyThreshold, double zcrThreshold);

    virtual std::vector<va::audio::VADProvider::VADResult> detectVoiceActivity(const char *buffer, size_t offset, size_t length) const override;

    virtual void release() override;

    virtual bool initialize(const va::audio::AudioConfig &audioConfig, int bufferSize) override;

private:
    int frameSizeMs_;
    int frameSizeSamples_; // derived from frameSizeMs_ and the sample rate
    double _energyThreshold;
    double zcrThreshold_;
    va::audio::AudioConfig _audioConfig;

    double calculateEnergy(const std::vector<float>& frame) const;

    double calculateZeroCrossingRate(const std::vector<float>& frame) const;
};

