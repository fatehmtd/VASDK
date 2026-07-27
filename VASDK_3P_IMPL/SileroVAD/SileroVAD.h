#pragma once

#include <VASDK/VADProvider.h>
#include <memory>
#include <vector>
#include <string>

// forward declaration of the VadIterator class
class VadIterator;

/**
 * @brief Voice activity detection provider using Silero VAD.
 */
class SileroVAD: public va::audio::VADProvider {
public:
    struct SileroVADConfig {
        int windows_frame_size;
        float threshold;
        int min_silence_duration_ms;
        int speech_pad_ms;
        int min_speech_duration_ms;

        SileroVADConfig()
            : windows_frame_size(32)
            , threshold(0.75f)
            , min_silence_duration_ms(100)
            , speech_pad_ms(30)
            , min_speech_duration_ms(150)
        {}
    };

    SileroVAD(const std::string& modelPath, SileroVAD::SileroVADConfig sileroVadConfig = SileroVAD::SileroVADConfig());
    ~SileroVAD();
    bool initialize(const va::audio::AudioConfig& audioConfig, int bufferSize) override;
    void release() override;
    std::vector<va::audio::VADProvider::VADResult> detectVoiceActivity(const char* buffer, size_t offset, size_t numSamples) const override;
    void resetState();
private:
    SileroVADConfig _sileroVadConfig;
    std::string _modelPath;
    std::unique_ptr<VadIterator> _vadIterator;
    int _bufferSize = 0; // retained to allocate local buffer in detectVoiceActivity
};
