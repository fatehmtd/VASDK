#pragma once

#include <vector>
#include <VASDK/VADProvider.h>
#include "vasdk_energy_zcr_vad_export.h"

namespace va::thirdparty::vad {

/** Settings of EnergyZCRVAD; every field has a default. */
struct EnergyZCRVADOptions {
    /** Length of one analysed frame. */
    int frameSizeMs = 20;
    /** A frame is loud enough for speech when its energy (sum of squared samples in -1..1) exceeds this.
     *  0.1 is about -34 dBFS RMS for a 20 ms frame at 16 kHz. */
    double energyThreshold = 0.1;
    /** A loud frame counts as speech only when its zero-crossing rate (0-1) is below this,
     *  which rejects hiss and other high-frequency noise. */
    double zeroCrossingRateThreshold = 0.3;
};

/**
 * @brief Voice activity from frame energy and zero-crossing rate: no model, for embedded targets.
 *
 *   auto vad = std::make_shared<va::thirdparty::vad::EnergyZCRVAD>();
 *   vad->initialize(audioConfig, bufferSize);
 */
class VASDK_ENERGY_ZCR_VAD_EXPORT EnergyZCRVAD : public va::audio::VADProvider {
public:
    explicit EnergyZCRVAD(EnergyZCRVADOptions options = {});

    std::vector<va::audio::VADProvider::VADResult> detectVoiceActivity(const char* buffer, size_t offset,
                                                                       size_t length) const override;

    void release() override;

    bool initialize(const va::audio::AudioConfig& audioConfig, int bufferSize) override;

private:
    EnergyZCRVADOptions _options;
    int _frameSizeSamples{1};  // derived from the frame length and the sample rate
    va::audio::AudioConfig _audioConfig;

    double calculateEnergy(const std::vector<float>& frame) const;

    double calculateZeroCrossingRate(const std::vector<float>& frame) const;
};

} // namespace va::thirdparty::vad
