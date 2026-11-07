#pragma once

#include <VASDK/GenericTranscriptionProvider.h>
#include <cartesiapp/cartesiapp.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/** Settings of CartesiappTranscriptionProvider; every field but the API key has a default. */
struct CartesiappTranscriptionOptions {
    /** API key of the service; required (the catalog falls back to the service's *_API_KEY variable). */
    std::string apiKey;
    /** Model to use. */
    std::string model = cartesiapp::stt::models::INK_WHISPER;
    /** Language code; empty lets the service detect it or use its default. */
    std::string language;
    /** PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). */
    std::string caBundlePath;
};

/**
 * @brief Real-time transcription provider backed by the Cartesia STT WebSocket API.
 */
class CartesiappTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit CartesiappTranscriptionProvider(CartesiappTranscriptionOptions options);
    ~CartesiappTranscriptionProvider() override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    size_t writeAudioData(const uint8_t* data, size_t size) override;
    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override;
    void commitEndOfSpeech() override;

    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override;
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

private:
    void connectCallbacks();
    void notifyStoppedOnce();

    static va::audio::TranscriptionResult buildResult(const cartesiapp::SttTranscriptEvent& event);
    static va::audio::TranscriptionResult buildResult(const cartesiapp::SttTranscriptionResult& result);

private:
    std::string _apiKey;
    std::string _model;
    std::string _language;
    std::string _caFilePath;

    std::unique_ptr<cartesiapp::SttRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::TranscriptionResultListener* _listener = nullptr;

    std::atomic<bool> _isRunning{ false };
    std::atomic<bool> _stopNotified{ false };
    mutable std::mutex _clientMutex;
};

} // namespace thirdparty
} // namespace va
