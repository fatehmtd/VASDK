#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <cartesiapp/cartesiapp.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time voice synthesis provider backed by the Cartesia TTS WebSocket API.
 */
class CartesiappVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    /// voiceId selects a voice from the caller's Cartesia account/catalog; there is
    /// no library-wide default since voice IDs are account-specific.
    explicit CartesiappVoiceSynthesisProvider(const std::string& apiKey,
                                               const std::string& voiceId,
                                               const std::string& model = cartesiapp::tts::models::SONIC_3_5,
                                               const std::string& language = {},
                                               const std::string& caFilePath = {});
    ~CartesiappVoiceSynthesisProvider() override;

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
    void connectCallbacks();
    cartesiapp::OutputFormat resolveOutputFormat() const;

private:
    std::string _apiKey;
    std::string _voiceId;
    std::string _model;
    std::string _language;
    std::string _caFilePath;

    std::unique_ptr<cartesiapp::TtsRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    std::atomic<bool> _isRunning{ false };

    mutable std::mutex _stateMutex;
    std::string _activeContextId;
    std::atomic<uint64_t> _contextCounter{ 1 };
};

} // namespace thirdparty
} // namespace va
