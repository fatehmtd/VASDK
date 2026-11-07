#pragma once

#include <VASDK/GenericTranscriptionProvider.h>
#include <fishaudiopp/fishaudiopp.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace va {
namespace thirdparty {

/** Settings of FishaudioppTranscriptionProvider; every field but the API key has a default. */
struct FishaudioppTranscriptionOptions {
    /** API key of the service; required (the catalog falls back to the service's *_API_KEY variable). */
    std::string apiKey;
    /** Language code; empty lets the service detect it or use its default. */
    std::string language;
    /** PEM file of certificates to trust; empty uses the TLS library's own (Android's mbedTLS can't read the system store). */
    std::string caBundlePath;
};

/**
 * @brief Transcription provider backed by the Fish Audio ASR REST API.
 *
 * Fish Audio's ASR endpoint (POST /v1/asr) is batch-only - there is no WebSocket
 * or other streaming variant. writeFullAudioData() calls it directly for
 * pre-segmented input (non-streaming mode). For continuous-stream input,
 * writeAudioData() buffers PCM until commitEndOfSpeech(), which hands the
 * buffered segment to a background worker thread that runs the same REST call
 * and delivers the result via the listener, so the audio-processing thread is
 * never blocked on the network round trip.
 */
class FishaudioppTranscriptionProvider : public va::audio::GenericTranscriptionProvider
{
public:
    explicit FishaudioppTranscriptionProvider(FishaudioppTranscriptionOptions options);
    ~FishaudioppTranscriptionProvider() override;

    bool initializeTranscriptionProvider() override;
    void releaseTranscriptionProvider() override;

    bool startTranscriptionProvider() override;
    void stopTranscriptionProvider() override;
    bool isTranscriptionProviderRunning() const override;

    size_t writeAudioData(const uint8_t* data, size_t size) override;
    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override;
    void commitEndOfSpeech() override;
    void clearAudioBuffer() override;

    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override;
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override;

    void setAudioConfiguration(const va::audio::AudioConfig& config) override;
    va::audio::AudioConfig getAudioConfiguration() const override;

private:
    void workerLoop();
    va::audio::TranscriptionResult transcribe(const std::vector<int16_t>& pcm16);

private:
    std::string _apiKey;
    std::string _language;
    std::string _caFilePath;

    va::audio::AudioConfig _audioConfig{};
    va::audio::TranscriptionResultListener* _listener = nullptr;

    std::atomic<bool> _isRunning{ false };

    // PCM16 audio accumulated since the last commit/clear (streaming-mode path).
    std::mutex _bufferMutex;
    std::vector<int16_t> _buffer;

    // Background worker: runs the (blocking) REST call off the audio-processing thread.
    std::thread _worker;
    std::mutex _workerMutex;
    std::condition_variable _workerCv;
    bool _workerShouldExit = false;
    bool _hasPendingSegment = false;
    std::vector<int16_t> _pendingSegment;
};

} // namespace thirdparty
} // namespace va
