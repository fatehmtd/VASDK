#pragma once
#include <gladiapp/gladiapp_ws.hpp>
#include <VASDK/GenericTranscriptionProvider.h>
#include <memory>

namespace va {
namespace thirdparty::stt {

/** Settings of GladiaPPTranscriptionProvider; every field but the API key has a default. */
struct GladiaPPTranscriptionOptions {
    /** API key of the service; required (the catalog falls back to the service's *_API_KEY variable). */
    std::string apiKey;
    /** PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). */
    std::string caBundlePath;
    /** Where the audio is processed: "us-west" or "eu-west". */
    std::string region = "us-west";
    /** Silence, in seconds, that ends an utterance. */
    double endpointingSeconds = 0.01;
    /** An utterance is cut after this many seconds without an endpoint. */
    int maxDurationWithoutEndpointingSeconds = 5;
};

/**
 * @brief Real-time transcription provider backed by the Gladia WebSocket API.
 */
class GladiaPPTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit GladiaPPTranscriptionProvider(GladiaPPTranscriptionOptions options);
    ~GladiaPPTranscriptionProvider() override;

    // GenericTranscriptionProvider interface
    void setTranscriptionResultListener(va::audio::TranscriptionResultListener *listener) override;
    va::audio::TranscriptionResultListener *getTranscriptionResultListener() const override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    size_t writeAudioData(const uint8_t *data, size_t size) override;

    void setAudioConfiguration(const va::audio::AudioConfig &config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

    gladiapp::v2::ws::request::InitializeSessionRequest getInitializeSessionRequest() const;

private:
    void connectSignals();

private:
    va::audio::TranscriptionResultListener* _listener = nullptr; // non-owning; lifetime managed by caller
    bool _isRunning = false;
    std::unique_ptr<gladiapp::v2::ws::GladiaWebsocketClient> _wsClient;
    va::audio::AudioConfig _audioConfig;
    std::string _apiKey;
    std::string _caFilePath;
    std::string _region;
    double _endpointingSeconds;
    int _maxDurationWithoutEndpointingSeconds;
    std::unique_ptr<gladiapp::v2::ws::GladiaWebsocketClientSession> _session{nullptr};
};

} // namespace thirdparty::stt
} // namespace va