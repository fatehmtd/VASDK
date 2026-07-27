#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <sonioxpp/soniox.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time voice synthesis provider backed by the Soniox TTS WebSocket API.
 */
class SonioxppVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider
{
public:
    SonioxppVoiceSynthesisProvider(const std::string& apiKey,
                                   const std::string& language = "en",
                                   const std::string& voice = "Adrian",
                                   const std::string& caFilePath = {});
    ~SonioxppVoiceSynthesisProvider() override;

    bool synthesizeSpeech(const std::string& text) override;
    void cancelSynthesis() override;

    bool startVoiceSynthesisProvider() override;
    void stopVoiceSynthesisProvider() override;
    bool isVoiceSynthesisProviderRunning() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener) override;
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override;

private:
    void connectCallbacks();
    std::string resolveAudioFormat() const;

private:
    std::string _apiKey;
    std::string _language;
    std::string _voice;
    std::string _caFilePath;

    std::unique_ptr<soniox::TtsRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    std::atomic<bool> _isRunning{ false };

    mutable std::mutex _stateMutex;
    std::string _activeStreamId;
    std::atomic<uint64_t> _streamCounter{ 1 };
};

} // namespace thirdparty
} // namespace va
