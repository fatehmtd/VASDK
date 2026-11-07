#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <sonioxpp/soniox.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty::tts {

/** Settings of SonioxppVoiceSynthesisProvider; every field but the API key has a default. */
struct SonioxppVoiceSynthesisOptions {
    /** API key of the service; required (the catalog falls back to the service's *_API_KEY variable). */
    std::string apiKey;
    /** Language code; empty lets the service detect it or use its default. */
    std::string language = "en";
    /** Voice to speak with. */
    std::string voice = "Adrian";
    /** PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). */
    std::string caBundlePath;
};

/**
 * @brief Real-time voice synthesis provider backed by the Soniox TTS WebSocket API.
 */
class SonioxppVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider
{
public:
    explicit SonioxppVoiceSynthesisProvider(SonioxppVoiceSynthesisOptions options);
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

} // namespace thirdparty::tts
} // namespace va
