#pragma once

#include <VASDK/GenericTranscriptionProvider.h>
#include <gradiumpp/gradium.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace va {
namespace thirdparty {

/**
 * @brief Real-time transcription provider backed by the Gradium ASR WebSocket API.
 */
class GradiumppTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit GradiumppTranscriptionProvider(
        const std::string& apiKey,
        const std::string& model = gradium::asr::models::default_model,
        const std::string& language = {});
    ~GradiumppTranscriptionProvider() override;

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
    std::string resolveInputFormat() const;

    static va::audio::TranscriptionResult buildResult(const gradium::TranscriptSegment& segment);
    static va::audio::TranscriptionResult parseNdjsonResult(const std::string& ndjson);

private:
    std::string _apiKey;
    std::string _model;
    std::string _language;

    std::unique_ptr<gradium::AsrRealtimeClient> _client;
    va::audio::AudioConfig _audioConfig{};
    va::audio::TranscriptionResultListener* _listener = nullptr;

    std::atomic<bool> _isRunning{ false };
    std::atomic<bool> _stopNotified{ false };
    std::atomic<int> _flushCounter{ 1 };
    mutable std::mutex _clientMutex;

    // Accumulates text/end_text segments across a turn; only cleared/finalized
    // on the matching "flushed" ack (see connectCallbacks()).
    std::mutex _pendingMutex;
    std::string _accumulatedText;
    gradium::TranscriptSegment _lastPartialSegment;
    bool _hasPendingPartial{ false };
    int _pendingFlushId{ -1 };

    std::mutex _readyMutex;
    std::condition_variable _readyCv;
    std::atomic<bool> _ready{ false };
    std::atomic<bool> _setupFailed{ false };
};

} // namespace thirdparty
} // namespace va
