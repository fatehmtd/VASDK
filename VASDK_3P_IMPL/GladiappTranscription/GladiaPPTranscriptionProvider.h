#pragma once
#include <gladiapp/gladiapp_ws.hpp>
#include <VASDK/GenericTranscriptionProvider.h>
#include <memory>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time transcription provider backed by the Gladia WebSocket API.
 */
class GladiaPPTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    GladiaPPTranscriptionProvider(const std::string &apiKey, const std::string &caFilePath = {});
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
    std::unique_ptr<gladiapp::v2::ws::GladiaWebsocketClientSession> _session{nullptr};
};

} // namespace thirdparty
} // namespace va