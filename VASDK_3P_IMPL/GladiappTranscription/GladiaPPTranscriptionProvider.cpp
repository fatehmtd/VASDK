#include "GladiaPPTranscriptionProvider.h"
#include <spdlog/spdlog.h>

using namespace gladiapp::v2::ws;
using namespace gladiapp::v2::response;

namespace va {
namespace thirdparty {

GladiaPPTranscriptionProvider::GladiaPPTranscriptionProvider(const std::string &apiKey, const std::string &caFilePath)
    : _apiKey(apiKey), _caFilePath(caFilePath)
{
}

GladiaPPTranscriptionProvider::~GladiaPPTranscriptionProvider()
{
    if (isTranscriptionProviderRunning())
    {
        stopTranscriptionProvider();
    }
    releaseTranscriptionProvider();
}

void GladiaPPTranscriptionProvider::setTranscriptionResultListener(va::audio::TranscriptionResultListener *listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener *GladiaPPTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

bool GladiaPPTranscriptionProvider::startTranscriptionProvider()
{
    return true;
}

void GladiaPPTranscriptionProvider::stopTranscriptionProvider()
{
    if(!_isRunning)
    {
        spdlog::warn("Transcription provider is not running.");
        return;
    }
    _isRunning = false;
    if (_session)
    {
        _session->sendStopSignal();
        _session->disconnect();
        _session.reset();
    }    
}

bool GladiaPPTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning;
}

bool GladiaPPTranscriptionProvider::initializeTranscriptionProvider()
{
    if (_wsClient)
    {
        spdlog::warn("WebSocket client is already initialized.");
        return true;
    }
    _wsClient = std::make_unique<gladiapp::v2::ws::GladiaWebsocketClient>(_apiKey, _caFilePath);

    gladiapp::v2::ws::request::InitializeSessionRequest initRequest = getInitializeSessionRequest();

    TranscriptionError error;

    _session.reset(_wsClient->connect(initRequest, &error));

    _isRunning = (_session != nullptr) && (error.status_code == 0);
    spdlog::info("WebSocket client initialized. isRunning: {}, error: {}", _isRunning, error.message);

    if(!_session->connectAndStart()) {
        spdlog::error("Failed to connect and start WebSocket session: {}", error.toString());
        _isRunning = false;
        _session.reset();
        return false;
    }

    connectSignals();

    return _isRunning;
}

void GladiaPPTranscriptionProvider::releaseTranscriptionProvider()
{
    if(!_wsClient)
    {
        spdlog::warn("Can't release: WebSocket client is not initialized.");
        return;
    }
    _wsClient.reset();
    _isRunning = false;
}

size_t GladiaPPTranscriptionProvider::writeAudioData(const uint8_t *data, size_t size)
{
    if (!_wsClient)
    {
        spdlog::error("WebSocket client is not initialized.");
        return size_t();
    }
    if (_session)
    {
        if(_session->sendAudioBinary(data, static_cast<int>(size))) {
            return size;
        }
        else {
            spdlog::error("Failed to send audio data.");
            return size_t();
        }
    } else {
        spdlog::error("WebSocket session is not initialized.");
    }
    return size_t();
}

void GladiaPPTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig &config)
{
    _audioConfig = config;
}

va::audio::AudioConfig GladiaPPTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

gladiapp::v2::ws::request::InitializeSessionRequest GladiaPPTranscriptionProvider::getInitializeSessionRequest() const
{
    gladiapp::v2::ws::request::InitializeSessionRequest initRequest;
    initRequest.channels = _audioConfig.channels;
    initRequest.endpointing = 0.01;
    initRequest.maximum_duration_without_endpointing = 5;

    gladiapp::v2::ws::request::InitializeSessionRequest::MessagesConfig messagesConfig;
    messagesConfig.receive_partial_transcripts = true;
    messagesConfig.receive_final_transcripts = true;
    messagesConfig.receive_speech_events = true;
    initRequest.messages_config = messagesConfig;

    switch (_audioConfig.sampleRate)
    {
    case 8000:
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_8000;
        break;
    case 16000:
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_16000;
        break;
    case 32000:
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_32000;
        break;
    case 44100:
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_44100;
        break;
    case 48000:
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_48000;
        break;
    default:
        spdlog::warn("Unsupported sample rate {}, defaulting to 16000 Hz", _audioConfig.sampleRate);
        initRequest.sample_rate = request::InitializeSessionRequest::SampleRate::SAMPLE_RATE_16000;
        break;
    }

    initRequest.region = request::InitializeSessionRequest::Region::US_WEST;
    switch (_audioConfig.format)
    {
    case va::audio::AudioFormat::BYTE:
        initRequest.bit_depth = request::InitializeSessionRequest::BitDepth::BIT_DEPTH_8;
        initRequest.encoding = request::InitializeSessionRequest::Encoding::WAV_PCM;
        break;
    case va::audio::AudioFormat::SHORT:
        initRequest.bit_depth = request::InitializeSessionRequest::BitDepth::BIT_DEPTH_16;
        initRequest.encoding = request::InitializeSessionRequest::Encoding::WAV_PCM;
        break;
    case va::audio::AudioFormat::INT:
        initRequest.bit_depth = request::InitializeSessionRequest::BitDepth::BIT_DEPTH_32;
        initRequest.encoding = request::InitializeSessionRequest::Encoding::WAV_PCM;
        break;
    case va::audio::AudioFormat::FLOAT:
        spdlog::warn("Float audio format is not directly supported, sending as 32-bit PCM");
        initRequest.bit_depth = request::InitializeSessionRequest::BitDepth::BIT_DEPTH_32;
        initRequest.encoding = request::InitializeSessionRequest::Encoding::WAV_PCM;
        break;
    default:
        spdlog::warn("Unsupported audio format {}, defaulting to 16-bit PCM", (int)_audioConfig.format);
        initRequest.bit_depth = request::InitializeSessionRequest::BitDepth::BIT_DEPTH_16;
        initRequest.encoding = request::InitializeSessionRequest::Encoding::WAV_PCM;
        break;
    }
    return initRequest;
}

void GladiaPPTranscriptionProvider::connectSignals()
{
    if (!_session)
    {
        spdlog::warn("WebSocket session is not initialized.");
        return;
    }
    _session->setOnConnectedCallback([this]() {
        spdlog::info("WebSocket connected.");
    });
    _session->setOnDisconnectedCallback([this]() {
        spdlog::info("WebSocket disconnected.");
        // keep reconnecting if the provider is still running
        if(isTranscriptionProviderRunning())
        {
            TranscriptionError error;
            _session.reset(_wsClient->connect(getInitializeSessionRequest(), &error));
            std::this_thread::sleep_for(std::chrono::seconds(1)); // wait before reconnecting
            if(_session && _session->connectAndStart())
            {
                spdlog::info("WebSocket reconnected.");
                connectSignals();
            } else {
                spdlog::error("Failed to reconnect WebSocket: {}", error.toString());
                _isRunning = false;
                if(_listener != nullptr)
                {
                    _listener->onTranscriptionError("WebSocket disconnected and failed to reconnect: " + error.toString());
                }
            }
        }
    });
    _session->setOnErrorCallback([this](const std::string &errorMessage) {
        spdlog::error("WebSocket error: {}", errorMessage);
        if(_listener != nullptr)
        {
            _listener->onTranscriptionError(errorMessage);
        }
    });
    _session->setOnTranscriptCallback([this](const response::Transcript &transcript) {
        if (_listener != nullptr)
        {            
            va::audio::TranscriptionResult result;
            result.text = transcript.data.utterance.text;
            result.confidence = static_cast<float>(transcript.data.utterance.confidence);
            result.startTimeMs = static_cast<int64_t>(transcript.data.utterance.start * 1000.0);
            result.endTimeMs = static_cast<int64_t>(transcript.data.utterance.end * 1000.0);
            if (!transcript.data.utterance.language.empty())
                result.languageTags.push_back({transcript.data.utterance.language, 1.0f});
            for(const auto& word : transcript.data.utterance.words) {
                va::audio::TranscriptionResult::Word resultWord;
                resultWord.text = word.word;
                resultWord.confidence = static_cast<float>(word.confidence);
                resultWord.startTimeMs = static_cast<int64_t>(word.start * 1000.0);
                resultWord.endTimeMs = static_cast<int64_t>(word.end * 1000.0);
                // Gladia WebSocket Word does not provide per-word alternatives or language tags
                result.words.push_back(resultWord);
            }
            if(transcript.data.is_final)
            {
                _listener->onFinalTranscription(result);
            } else {
                _listener->onPartialTranscription(result);
            }
        }
    });
    _session->setOnSpeechStartedCallback([this](const response::SpeechEvent &event) {
        //spdlog::info("Speech started event received.");
        if (_listener != nullptr)
        {
            _listener->onTranscriptionStarted();
        }
    });
    _session->setOnSpeechEndedCallback([this](const response::SpeechEvent &event) {
        //spdlog::info("Speech ended event received.");
        if (_listener != nullptr)
        {
            _listener->onTranscriptionStopped();
        }
    });
}

} // namespace thirdparty
} // namespace va
