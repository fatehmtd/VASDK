#include "FishaudioppVoiceSynthesisProvider.h"
#include <spdlog/spdlog.h>

namespace va {
namespace thirdparty {

FishaudioppVoiceSynthesisProvider::FishaudioppVoiceSynthesisProvider(
    const std::string& apiKey,
    const std::string& referenceId,
    const std::string& model,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _referenceId(referenceId),
      _model(model),
      _caFilePath(caFilePath)
{
}

FishaudioppVoiceSynthesisProvider::~FishaudioppVoiceSynthesisProvider()
{
    stopVoiceSynthesisProvider();
}

void FishaudioppVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
    if (config.format != va::audio::AudioFormat::SHORT) {
        // Fish Audio's live "pcm" output format is 16-bit signed PCM; other formats
        // will play back incorrectly since there is no float32/byte PCM variant to request.
        spdlog::warn("[FishAudioTTS] Fish Audio live TTS only outputs 16-bit PCM; audio format {} "
                     "may not play back correctly", static_cast<int>(config.format));
    }
}

va::audio::AudioConfig FishaudioppVoiceSynthesisProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

bool FishaudioppVoiceSynthesisProvider::synthesizeSpeech(const std::string& text)
{
    if (text.empty()) {
        return false;
    }

    if (_apiKey.empty()) {
        spdlog::error("[FishAudioTTS] FISHAUDIO_API_KEY is empty; cannot synthesize speech");
        return false;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);

    if (!_client || !_client->isOpen()) {
        if (!connectClientLocked()) {
            spdlog::error("[FishAudioTTS] Provider is not connected and could not reconnect");
            return false;
        }
    }

    fishaudiopp::tts::TtsRequest request;
    request.model = _model.empty() ? fishaudiopp::tts::models::S1 : _model;
    // Deliberately leave request.text empty: TtsRequest is shared with the REST client,
    // where .text is the only way to supply text, but the live protocol's StartEvent
    // treats it as an initial text chunk. Since sendText(text) below already delivers
    // the full text, also setting request.text here fed the server the same text twice,
    // which it dutifully synthesized twice (heard as the whole utterance repeating).
    if (!_referenceId.empty()) {
        request.reference_id = _referenceId;
    }
    request.format = fishaudiopp::tts::formats::PCM;
    request.sample_rate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 24000;

    try {
        _client->start(request);
        _client->sendText(text);
        _client->stop();
        // Fish Audio's live protocol has no explicit "started" acknowledgement event
        // (only "audio" and "finish"), so fire this synchronously like the REST-backed
        // providers do.
        if (_resultListener) {
            _resultListener->onSynthesisStarted();
        }
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[FishAudioTTS] synthesizeSpeech failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }
}

void FishaudioppVoiceSynthesisProvider::cancelSynthesis()
{
    if (!_isRunning.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    // Fish Audio's live protocol has no mid-stream cancel event - the only way to stop
    // generation immediately is to drop the connection. synthesizeSpeech() reconnects
    // lazily on the next call.
    if (_client) {
        try {
            _client->close();
        }
        catch (...) {
        }
    }
    _isRunning.store(false);

    if (_resultListener) {
        _resultListener->onSynthesisCompleted();
    }
}

bool FishaudioppVoiceSynthesisProvider::startVoiceSynthesisProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[FishAudioTTS] FISHAUDIO_API_KEY is empty; cannot start provider");
        return false;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    return connectClientLocked();
}

void FishaudioppVoiceSynthesisProvider::stopVoiceSynthesisProvider()
{
    if (!_isRunning.exchange(false)) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        try {
            _client->close();
        }
        catch (...) {
        }
        _client.reset();
    }
}

bool FishaudioppVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const
{
    return _isRunning.load();
}

void FishaudioppVoiceSynthesisProvider::setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener)
{
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener* FishaudioppVoiceSynthesisProvider::getVoiceSynthesisResultListener() const
{
    return _resultListener;
}

bool FishaudioppVoiceSynthesisProvider::connectClientLocked()
{
    _client = std::make_unique<fishaudiopp::tts::TtsLiveClient>(
        _apiKey,
        _model.empty() ? fishaudiopp::tts::models::S1 : _model,
        nullptr,
        std::string(fishaudiopp::FISH_AUDIO_WS_BASE_URL) + fishaudiopp::tts::TTS_LIVE_ENDPOINT_PATH,
        _caFilePath);
    connectCallbacks();

    try {
        _client->connect();
    }
    catch (const std::exception& e) {
        spdlog::error("[FishAudioTTS] Failed to connect to the Fish Audio live TTS API: {}", e.what());
        _client.reset();
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }

    _isRunning.store(true);
    return true;
}

void FishaudioppVoiceSynthesisProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnAudio([this](const fishaudiopp::tts::TtsLiveAudioEvent& event) {
        if (_resultListener && !event.audio.empty()) {
            _resultListener->onAudioDataReceived(
                reinterpret_cast<const char*>(event.audio.data()), event.audio.size());
        }
    });

    _client->setOnFinish([this](const fishaudiopp::tts::TtsLiveFinishEvent& event) {
        if (!_resultListener) {
            return;
        }
        if (event.reason == "error") {
            _resultListener->onSynthesisError("Fish Audio TTS session finished with an error");
        }
        else {
            _resultListener->onSynthesisCompleted();
        }
    });

    _client->setOnTransportError([this](const std::string& error) {
        if (_resultListener) {
            _resultListener->onSynthesisError(error);
        }
    });

    _client->setOnClosed([this] {
        _isRunning.store(false);
    });
}

} // namespace thirdparty
} // namespace va
