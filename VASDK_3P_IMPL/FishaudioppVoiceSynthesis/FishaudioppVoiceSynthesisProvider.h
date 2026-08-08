#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <fishaudiopp/fishaudiopp.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

    // Fish Audio voice ids for reference.
    namespace fishaudio::voices {
        constexpr const char* ETHAN_VOICE_ID = "536d3a5e000945adb7038665781a4aca";
        constexpr const char* SELENE_VOICE_ID = "b347db033a6549378b48d00acb0d06cd";
    };

/**
 * @brief Real-time voice synthesis provider backed by the Fish Audio live TTS WebSocket API.
 */
class FishaudioppVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    explicit FishaudioppVoiceSynthesisProvider(const std::string& apiKey,
                                                const std::string& referenceId = fishaudio::voices::ETHAN_VOICE_ID,
                                                const std::string& model = fishaudiopp::tts::models::S1,
                                                const std::string& caFilePath = {});
    ~FishaudioppVoiceSynthesisProvider() override;

    void setAudioConfiguration(const va::audio::AudioConfig &config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    bool synthesizeSpeech(const std::string& text) override;
    void cancelSynthesis() override;

    bool startVoiceSynthesisProvider() override;
    void stopVoiceSynthesisProvider() override;
    bool isVoiceSynthesisProviderRunning() const override;

    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener *listener) override;
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override;

private:
    // Caller must hold _clientMutex.
    bool connectClientLocked();
    void connectCallbacks();

private:
    std::string _apiKey;
    std::string _referenceId;
    std::string _model;
    std::string _caFilePath;

    std::unique_ptr<fishaudiopp::tts::TtsLiveClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    std::atomic<bool> _isRunning{ false };

    mutable std::mutex _clientMutex;
};

} // namespace thirdparty
} // namespace va
