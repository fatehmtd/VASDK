#include "CartesiappVoiceSynthesisProvider.h"

#include <spdlog/spdlog.h>

namespace va {
namespace thirdparty {

CartesiappVoiceSynthesisProvider::CartesiappVoiceSynthesisProvider(
    const std::string& apiKey,
    const std::string& voiceId,
    const std::string& model,
    const std::string& language,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _voiceId(voiceId),
      _model(model),
      _language(language),
      _caFilePath(caFilePath)
{
}

CartesiappVoiceSynthesisProvider::~CartesiappVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

void CartesiappVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig CartesiappVoiceSynthesisProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

bool CartesiappVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    if (!_isRunning.load()) {
        if (!startVoiceSynthesisProvider()) {
            spdlog::error("[CartesiaTTS] Provider is not running and could not be restarted");
            return false;
        }
    }

    const std::string contextId = "vasdk-cartesia-" + std::to_string(_contextCounter.fetch_add(1));

    cartesiapp::TtsGenerationRequest request;
    request.model_id = _model.empty() ? cartesiapp::tts::models::SONIC_3_5 : _model;
    request.transcript = text;
    request.voice.id = _voiceId;
    request.output_format = resolveOutputFormat();
    if (!_language.empty()) {
        request.language = _language;
    }
    request.context_id = contextId;
    request.continue_ = false;

    try {
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _activeContextId = contextId;
        }

        _client->sendGeneration(request);

        if (_resultListener) {
            _resultListener->onSynthesisStarted();
        }
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[CartesiaTTS] synthesizeSpeech failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }
}

void CartesiappVoiceSynthesisProvider::cancelSynthesis() {
    if (!_isRunning.load() || !_client) {
        return;
    }

    std::string contextId;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        contextId = _activeContextId;
        _activeContextId.clear();
    }

    if (contextId.empty()) {
        return;
    }

    try {
        _client->cancelContext(contextId);
    }
    catch (const std::exception& e) {
        spdlog::warn("[CartesiaTTS] cancelContext failed: {}", e.what());
    }

    if (_resultListener) {
        _resultListener->onSynthesisCompleted();
    }
}

bool CartesiappVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[CartesiaTTS] CARTESIA_API_KEY is empty; cannot start provider");
        return false;
    }

    if (_voiceId.empty()) {
        spdlog::error("[CartesiaTTS] voiceId is empty; cannot start provider");
        return false;
    }

    _client = std::make_unique<cartesiapp::TtsRealtimeClient>(
        _apiKey,
        cartesiapp::api_versions::LATEST,
        nullptr,
        std::string(cartesiapp::CARTESIA_WS_BASE_URL) + "/tts/websocket",
        _caFilePath);
    connectCallbacks();

    try {
        _client->connect();
    }
    catch (const std::exception& e) {
        spdlog::error("[CartesiaTTS] connect() failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        _client.reset();
        return false;
    }

    _isRunning.store(true);
    return true;
}

void CartesiappVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
    if (!_isRunning.exchange(false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _activeContextId.clear();
    }

    if (_client) {
        try {
            _client->close();
        }
        catch (...) {
        }
        _client.reset();
    }
}

bool CartesiappVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _isRunning.load();
}

void CartesiappVoiceSynthesisProvider::setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener)
{
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener* CartesiappVoiceSynthesisProvider::getVoiceSynthesisResultListener() const
{
    return _resultListener;
}

void CartesiappVoiceSynthesisProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnEvent([this](const cartesiapp::TtsStreamEvent& event) {
        if (event.type == "chunk") {
            if (_resultListener && !event.audio.empty()) {
                _resultListener->onAudioDataReceived(
                    reinterpret_cast<const char*>(event.audio.data()), event.audio.size());
            }
            return;
        }

        if (event.type == "done") {
            bool isActiveContext = false;
            {
                std::lock_guard<std::mutex> lock(_stateMutex);
                if (event.context_id.has_value() && *event.context_id == _activeContextId) {
                    _activeContextId.clear();
                    isActiveContext = true;
                }
            }
            if (isActiveContext && _resultListener) {
                _resultListener->onSynthesisCompleted();
            }
            return;
        }

        if (event.type == "error") {
            if (_resultListener) {
                _resultListener->onSynthesisError(event.message.empty() ? event.title : event.message);
            }
        }
    });

    _client->setOnError([this](const std::string& error) {
        if (_resultListener) {
            _resultListener->onSynthesisError(error);
        }
    });

    _client->setOnClosed([this]() {
        spdlog::info("[CartesiaTTS] Disconnected.");
        _isRunning.store(false);
    });
}

cartesiapp::OutputFormat CartesiappVoiceSynthesisProvider::resolveOutputFormat() const
{
    cartesiapp::OutputFormat format;
    format.container = cartesiapp::tts::container::RAW;
    format.sample_rate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : cartesiapp::sample_rate::SR_24000;

    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT:
        format.encoding = cartesiapp::tts::encoding::PCM_S16LE;
        break;
    case va::audio::AudioFormat::FLOAT:
        format.encoding = cartesiapp::tts::encoding::PCM_F32LE;
        break;
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::INT:
    case va::audio::AudioFormat::UNKNOWN:
    default:
        spdlog::warn("[CartesiaTTS] Unsupported audio format {} for direct output; using pcm_s16le",
                     static_cast<int>(_audioConfig.format));
        format.encoding = cartesiapp::tts::encoding::PCM_S16LE;
        break;
    }

    return format;
}

} // namespace thirdparty
} // namespace va
