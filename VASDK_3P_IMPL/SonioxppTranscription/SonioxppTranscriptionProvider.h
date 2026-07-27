#pragma once

#include <VASDK/GenericTranscriptionProvider.h>
#include <sonioxpp/soniox.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time transcription provider backed by the Soniox STT WebSocket API.
 */
class SonioxppTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit SonioxppTranscriptionProvider(const std::string& apiKey, const std::string& caFilePath = {});
    ~SonioxppTranscriptionProvider() override;

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
    bool ensureRealtimeSessionStarted();
    void notifyStoppedOnce();

    static va::audio::TranscriptionResult buildResultFromTokens(
        const std::vector<soniox::Token>& tokens,
        bool includeFinal,
        bool includeNonFinal);

private:
    std::string _apiKey;
    std::string _caFilePath;
    std::unique_ptr<soniox::SttRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::TranscriptionResultListener* _listener = nullptr;

    std::atomic<bool> _isRunning{ false };
    std::atomic<bool> _stopNotified{ false };
    std::atomic<bool> _sessionStarted{ false };
    mutable std::mutex _clientMutex;
};

} // namespace thirdparty
} // namespace va
