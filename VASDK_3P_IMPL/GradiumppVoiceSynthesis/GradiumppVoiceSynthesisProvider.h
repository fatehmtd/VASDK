#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <gradiumpp/gradium.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time voice synthesis provider backed by the Gradium TTS WebSocket API.
 */
class GradiumppVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    /// voiceId selects a voice from Gradium's catalog (see gradium::voices::*); there
    /// is no library-wide default since voice choice is a caller/account decision.
    explicit GradiumppVoiceSynthesisProvider(const std::string& apiKey,
                                              const std::string& voiceId,
                                              const std::string& model = gradium::tts::models::default_model);
    ~GradiumppVoiceSynthesisProvider() override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    bool synthesizeSpeech(const std::string& text) override;
    void cancelSynthesis() override;

    bool startVoiceSynthesisProvider() override;
    void stopVoiceSynthesisProvider() override;
    bool isVoiceSynthesisProviderRunning() const override;

    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener) override;
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override;

private:
    void connectCallbacks();
    std::string resolveOutputFormat() const;

private:
    std::string _apiKey;
    std::string _voiceId;
    std::string _model;

    std::unique_ptr<gradium::TtsRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    std::atomic<bool> _isRunning{ false };
    std::atomic<bool> _synthesisActive{ false };
    mutable std::mutex _clientMutex;

    // Unique client_req_id per synthesizeSpeech() call; see the .cpp for why.
    std::atomic<int> _requestCounter{ 1 };
};

} // namespace thirdparty
} // namespace va
