#include "DeepgramppVoiceSynthesisProvider.h"
#include <spdlog/spdlog.h>

namespace va {
namespace thirdparty {

DeepgramppVoiceSynthesisProvider::DeepgramppVoiceSynthesisProvider(
    const std::string& apiKey,
    const std::string& model,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _model(model),
      _caFilePath(caFilePath)
{
}

DeepgramppVoiceSynthesisProvider::~DeepgramppVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

void DeepgramppVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig DeepgramppVoiceSynthesisProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

bool DeepgramppVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    if (!_isRunning.load()) {
        if (!startVoiceSynthesisProvider()) {
            spdlog::error("[DeepgramTTS] Provider is not running and could not be restarted");
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (!_deepgramClient) {
        return false;
    }

    try {
        if (!_deepgramClient->speak(text)) {
            spdlog::error("[DeepgramTTS] speak() failed");
            return false;
        }
        if (!_deepgramClient->sendFlushMessage()) {
            spdlog::error("[DeepgramTTS] sendFlushMessage failed");
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[DeepgramTTS] synthesizeSpeech failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }
}

void DeepgramppVoiceSynthesisProvider::cancelSynthesis() {
    if (!_isRunning.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (!_deepgramClient) {
        return;
    }

    try {
        _deepgramClient->sendClearMessage();
    }
    catch (const std::exception& e) {
        spdlog::warn("[DeepgramTTS] sendClearMessage failed: {}", e.what());
    }

    if (_resultListener) {
        _resultListener->onSynthesisCompleted();
    }
}

bool DeepgramppVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[DeepgramTTS] DEEPGRAM_API_KEY is empty; cannot start provider");
        return false;
    }

    std::string encoding;
    try {
        encoding = resolveAudioFormat();
    }
    catch (const std::exception& e) {
        spdlog::error("[DeepgramTTS] {}", e.what());
        return false;
    }

    deepgram::speak::LiveSpeakConfig liveSpeakConfig;
    liveSpeakConfig.model = _model.empty() ? deepgram::speak::models::aura_2::en::THALIA : _model;
    liveSpeakConfig.encoding = encoding;
    liveSpeakConfig.sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;

    std::lock_guard<std::mutex> lock(_clientMutex);

    _deepgramClient = std::make_unique<deepgram::speak::SpeakWebsocketClient>(_apiKey, nullptr, _caFilePath);
    connectCallbacks();

    if (!_deepgramClient->connect(liveSpeakConfig)) {
        spdlog::error("[DeepgramTTS] connect() failed");
        _deepgramClient.reset();
        return false;
    }

    // Arms the end-of-speech idle-timeout monitor; Deepgram never sends an
    // explicit "speech ended" event, so onSynthesisCompleted relies on it.
    _deepgramClient->startReceiving();

    _isRunning.store(true);
    return true;
}

void DeepgramppVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
    if (!_isRunning.exchange(false)) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_deepgramClient) {
        try {
            _deepgramClient->sendCloseMessage();
            _deepgramClient->close();
        }
        catch (...) {
        }
        _deepgramClient.reset();
    }
}

bool DeepgramppVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _isRunning.load();
}

void DeepgramppVoiceSynthesisProvider::setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener)
{
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener* DeepgramppVoiceSynthesisProvider::getVoiceSynthesisResultListener() const
{
    return _resultListener;
}

void DeepgramppVoiceSynthesisProvider::connectCallbacks()
{
    if (!_deepgramClient) {
        return;
    }

    _deepgramClient->setSpeechErrorCallback([this](const std::string& error) {
        if (_resultListener) {
            _resultListener->onSynthesisError(error);
        }
    });
    _deepgramClient->setSpeechDisconnectedCallback([this]() {
        spdlog::info("[DeepgramTTS] Disconnected.");
        _isRunning.store(false);
    });
    _deepgramClient->setSpeechStartedCallback([this]() {
        if (_resultListener) {
            _resultListener->onSynthesisStarted();
        }
    });
    _deepgramClient->setSpeechEndedCallback([this]() {
        if (_resultListener) {
            _resultListener->onSynthesisCompleted();
        }
    });
    _deepgramClient->setSpeechCloseFrameCallback([this](const deepgram::speak::SpeakCloseFrame&) {
        _isRunning.store(false);
    });
    _deepgramClient->setSpeechResultCallback([this](const char* data, int length) {
        if (_resultListener) {
            _resultListener->onAudioDataReceived(data, length);
        }
    });
    _deepgramClient->setSpeechReceptionTimeout(250);
}

std::string DeepgramppVoiceSynthesisProvider::resolveAudioFormat() const
{
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT:
        return deepgram::speak::encoding::LINEAR_16;
    case va::audio::AudioFormat::FLOAT:
        return deepgram::speak::encoding::LINEAR_32;
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::INT:
    case va::audio::AudioFormat::UNKNOWN:
    default:
        throw std::runtime_error(
            "unsupported audio format " + std::to_string(static_cast<int>(_audioConfig.format)) +
            " for direct Deepgram TTS output; only SHORT (linear16) and FLOAT (linear32) are supported");
    }
}

} // namespace thirdparty
} // namespace va
