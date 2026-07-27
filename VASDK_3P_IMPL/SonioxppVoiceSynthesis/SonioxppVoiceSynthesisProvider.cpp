#include "SonioxppVoiceSynthesisProvider.h"

#include <cppcodec/base64_rfc4648.hpp>
#include <sonioxpp/api_constants.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> decodeBase64(const std::string& input)
{
    return cppcodec::base64_rfc4648::decode<std::vector<uint8_t>>(input);
}

} // namespace

namespace va {
namespace thirdparty {

SonioxppVoiceSynthesisProvider::SonioxppVoiceSynthesisProvider(
    const std::string& apiKey,
    const std::string& language,
    const std::string& voice,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _language(language),
      _voice(voice),
      _caFilePath(caFilePath)
{
}

SonioxppVoiceSynthesisProvider::~SonioxppVoiceSynthesisProvider()
{
    stopVoiceSynthesisProvider();
}

bool SonioxppVoiceSynthesisProvider::synthesizeSpeech(const std::string& text)
{
    if (!_isRunning.load()) {
        if (!startVoiceSynthesisProvider()) {
            spdlog::error("[SonioxTTS] Provider is not running and could not be restarted");
            return false;
        }
    }
    if (text.empty()) {
        return false;
    }

    const std::string streamId = "vasdk-stream-" + std::to_string(_streamCounter.fetch_add(1));

    soniox::TtsRealtimeStreamConfig cfg;
    cfg.api_key = _apiKey;
    cfg.stream_id = streamId;
    cfg.model = soniox::tts::models::realtime_v1;
    cfg.language = _language.empty() ? "en" : _language;
    cfg.voice = _voice.empty() ? "Adrian" : _voice;
    cfg.audio_format = resolveAudioFormat();
    if (_audioConfig.sampleRate > 0) {
        cfg.sample_rate = _audioConfig.sampleRate;
    }

    try {
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _activeStreamId = streamId;
        }

        _client->startStream(cfg);
        _client->sendText(streamId, text, true);

        if (_resultListener) {
            _resultListener->onSynthesisStarted();
        }
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[SonioxTTS] Failed to start stream: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }
}

void SonioxppVoiceSynthesisProvider::cancelSynthesis()
{
    if (!_isRunning.load() || !_client) {
        return;
    }

    std::string streamId;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        streamId = _activeStreamId;
        _activeStreamId.clear();
    }

    if (streamId.empty()) {
        return;
    }

    try {
        _client->cancelStream(streamId);
    }
    catch (const std::exception& e) {
        spdlog::warn("[SonioxTTS] cancelStream failed: {}", e.what());
    }

    if (_resultListener) {
        _resultListener->onSynthesisCompleted();
    }
}

bool SonioxppVoiceSynthesisProvider::startVoiceSynthesisProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[SonioxTTS] SONIOX_API_KEY is empty; cannot start provider");
        return false;
    }

    _client = std::make_unique<soniox::TtsRealtimeClient>(nullptr, "wss://tts-rt.soniox.com/tts-websocket", _caFilePath);
    connectCallbacks();

    try {
        _client->connect();
        _isRunning.store(true);
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[SonioxTTS] connect() failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        _client.reset();
        return false;
    }
}

void SonioxppVoiceSynthesisProvider::stopVoiceSynthesisProvider()
{
    if (!_isRunning.exchange(false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _activeStreamId.clear();
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

bool SonioxppVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const
{
    return _isRunning.load();
}

void SonioxppVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig SonioxppVoiceSynthesisProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void SonioxppVoiceSynthesisProvider::setVoiceSynthesisResultListener(
    va::audio::VoiceSynthesisResultListener* listener)
{
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener* SonioxppVoiceSynthesisProvider::getVoiceSynthesisResultListener() const
{
    return _resultListener;
}

void SonioxppVoiceSynthesisProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnParsedMessage([this](const soniox::TtsRealtimeMessage& message) {
        if (_resultListener && !message.audio.empty()) {
            try {
                auto bytes = decodeBase64(message.audio);
                if (!bytes.empty()) {
                    _resultListener->onAudioDataReceived(
                        reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
            }
            catch (const std::exception& e) {
                _resultListener->onSynthesisError(e.what());
            }
        }

        if (_resultListener && (message.error_code != 0 || !message.error_message.empty())) {
            _resultListener->onSynthesisError(
                message.error_message.empty() ? "Unknown Soniox TTS stream error" : message.error_message);
        }

        if (message.terminated) {
            bool isActiveStream = false;
            {
                std::lock_guard<std::mutex> lock(_stateMutex);
                if (message.stream_id == _activeStreamId) {
                    _activeStreamId.clear();
                    isActiveStream = true;
                }
            }

            if (isActiveStream && _resultListener) {
                _resultListener->onSynthesisCompleted();
            }
        }
    });

    _client->setOnError([this](const std::string& error) {
        if (_resultListener) {
            _resultListener->onSynthesisError(error);
        }
    });

    _client->setOnClosed([this]() {
        _isRunning.store(false);
    });
}

std::string SonioxppVoiceSynthesisProvider::resolveAudioFormat() const
{
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT:
        return soniox::tts::audio_formats::pcm_s16le;
    case va::audio::AudioFormat::FLOAT:
        return soniox::tts::audio_formats::pcm_f32le;
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::INT:
    default:
        spdlog::warn("[SonioxTTS] Unsupported audio format {} for direct output; using pcm_s16le",
                     static_cast<int>(_audioConfig.format));
        return soniox::tts::audio_formats::pcm_s16le;
    }
}

} // namespace thirdparty
} // namespace va
