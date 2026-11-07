#pragma once

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <deepgrampp/deepgram.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty::tts {

/** Settings of DeepgramppVoiceSynthesisProvider; every field but the API key has a default. */
struct DeepgramppVoiceSynthesisOptions {
    /** API key of the service; required (the catalog falls back to the service's *_API_KEY variable). */
    std::string apiKey;
    /** Model to use. */
    std::string model = deepgram::speak::models::aura_2::en::THALIA;
    /** PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). */
    std::string caBundlePath;
};

/**
 * @brief Real-time voice synthesis provider backed by the Deepgram Speak WebSocket API.
 */
class DeepgramppVoiceSynthesisProvider : public va::audio::GenericVoiceSynthesisProvider {
public:
    explicit DeepgramppVoiceSynthesisProvider(DeepgramppVoiceSynthesisOptions options);
    ~DeepgramppVoiceSynthesisProvider() override;

    void setAudioConfiguration(const va::audio::AudioConfig &config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    bool synthesizeSpeech(const std::string& text) override;
    void cancelSynthesis() override;
    // Speak messages on one socket are synthesized in order.
    bool acceptsQueuedSynthesis() const override { return true; }

    bool startVoiceSynthesisProvider() override;
    void stopVoiceSynthesisProvider() override;
    bool isVoiceSynthesisProviderRunning() const override;

    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener *listener) override;
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override;

private:
    void connectCallbacks();
    std::string resolveAudioFormat() const;

private:
    std::string _apiKey;
    std::string _model;
    std::string _caFilePath;

    std::unique_ptr<deepgram::speak::SpeakWebsocketClient> _deepgramClient;
    va::audio::AudioConfig _audioConfig{};
    va::audio::VoiceSynthesisResultListener* _resultListener = nullptr;
    std::atomic<bool> _isRunning{ false };

    mutable std::mutex _clientMutex;
};

} // namespace thirdparty::tts
} // namespace va
