#include "GradiumppVoiceSynthesisProvider.h"

#include <cppcodec/base64_rfc4648.hpp>
#include <spdlog/spdlog.h>

namespace va {
namespace thirdparty {

GradiumppVoiceSynthesisProvider::GradiumppVoiceSynthesisProvider(
    const std::string& apiKey,
    const std::string& voiceId,
    const std::string& model)
    : _apiKey(apiKey),
      _voiceId(voiceId),
      _model(model)
{
}

GradiumppVoiceSynthesisProvider::~GradiumppVoiceSynthesisProvider()
{
    stopVoiceSynthesisProvider();
}

void GradiumppVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig GradiumppVoiceSynthesisProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

std::string GradiumppVoiceSynthesisProvider::resolveOutputFormat() const
{
    switch (_audioConfig.sampleRate) {
    case 8000: return gradium::tts::output_formats::pcm_8000;
    case 16000: return gradium::tts::output_formats::pcm_16000;
    case 22050: return gradium::tts::output_formats::pcm_22050;
    case 24000: return gradium::tts::output_formats::pcm_24000;
    case 44100: return gradium::tts::output_formats::pcm_44100;
    case 48000: return gradium::tts::output_formats::pcm_48000;
    default: return gradium::tts::output_formats::pcm; // 48 kHz default
    }
}

bool GradiumppVoiceSynthesisProvider::synthesizeSpeech(const std::string& text)
{
    if (text.empty()) {
        return false;
    }

    if (!_isRunning.load()) {
        if (!startVoiceSynthesisProvider()) {
            spdlog::error("[GradiumTTS] Provider is not running and could not be restarted");
            return false;
        }
    }

    try {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return false;
        }

        // Each request needs its own setup(), even on a kept-alive connection -
        // reusing an already-completed stream is silently dropped by the server.
        const std::string reqId = "req-" + std::to_string(_requestCounter.fetch_add(1));
        gradium::TtsRealtimeSetup setup;
        setup.voice_id = _voiceId;
        setup.model_name = _model.empty() ? gradium::tts::models::default_model : _model;
        setup.output_format = resolveOutputFormat();
        setup.client_req_id = reqId;
        setup.close_ws_on_eos = false;
        _client->setup(setup);

        _synthesisActive.store(true);
        _client->sendText(text, reqId);
        _client->sendEndOfStream(reqId);

        if (_resultListener) {
            _resultListener->onSynthesisStarted();
        }
        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[GradiumTTS] synthesizeSpeech failed: {}", e.what());
        _synthesisActive.store(false);
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        return false;
    }
}

void GradiumppVoiceSynthesisProvider::cancelSynthesis()
{
    if (!_synthesisActive.exchange(false)) {
        return;
    }

    // No in-flight abort message exists; dropping the connection is the only
    // way to stop emitted audio immediately.
    {
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

    _isRunning.store(false);

    if (_resultListener) {
        _resultListener->onSynthesisCompleted();
    }
}

bool GradiumppVoiceSynthesisProvider::startVoiceSynthesisProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[GradiumTTS] GRADIUM_API_KEY is empty; cannot start provider");
        return false;
    }

    if (_voiceId.empty()) {
        spdlog::error("[GradiumTTS] voiceId is empty; cannot start provider");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        _client = std::make_unique<gradium::TtsRealtimeClient>();
        connectCallbacks();
    }

    try {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return false;
        }
        _client->connect(_apiKey);
    }
    catch (const std::exception& e) {
        spdlog::error("[GradiumTTS] connect() failed: {}", e.what());
        if (_resultListener) {
            _resultListener->onSynthesisError(e.what());
        }
        std::lock_guard<std::mutex> lock(_clientMutex);
        _client.reset();
        return false;
    }

    _isRunning.store(true);
    return true;
}

void GradiumppVoiceSynthesisProvider::stopVoiceSynthesisProvider()
{
    if (!_isRunning.exchange(false)) {
        return;
    }

    _synthesisActive.store(false);

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

bool GradiumppVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const
{
    return _isRunning.load();
}

void GradiumppVoiceSynthesisProvider::setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener)
{
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener* GradiumppVoiceSynthesisProvider::getVoiceSynthesisResultListener() const
{
    return _resultListener;
}

void GradiumppVoiceSynthesisProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnParsedMessage([this](const gradium::TtsRealtimeMessage& message) {
        switch (message.type) {
        case gradium::TtsRealtimeMessage::Type::Ready:
            break;
        case gradium::TtsRealtimeMessage::Type::Audio: {
            if (!_synthesisActive.load() || message.audio_base64.empty()) {
                break;
            }
            try {
                const auto bytes = cppcodec::base64_rfc4648::decode(message.audio_base64);
                if (_resultListener && !bytes.empty()) {
                    _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
            }
            catch (const std::exception& e) {
                spdlog::warn("[GradiumTTS] Failed to decode audio chunk: {}", e.what());
            }
            break;
        }
        case gradium::TtsRealtimeMessage::Type::EndOfStream: {
            if (_synthesisActive.exchange(false) && _resultListener) {
                _resultListener->onSynthesisCompleted();
            }
            break;
        }
        case gradium::TtsRealtimeMessage::Type::Error: {
            if (_synthesisActive.exchange(false) && _resultListener) {
                _resultListener->onSynthesisError(message.error_message);
            }
            break;
        }
        case gradium::TtsRealtimeMessage::Type::Text:
        case gradium::TtsRealtimeMessage::Type::UNKNOWN:
        default:
            break;
        }
    });

    _client->setOnError([this](const std::string& error) {
        if (_synthesisActive.exchange(false) && _resultListener) {
            _resultListener->onSynthesisError(error);
        }
    });

    _client->setOnClosed([this]() {
        spdlog::info("[GradiumTTS] Disconnected.");
        _isRunning.store(false);
    });
}

} // namespace thirdparty
} // namespace va
