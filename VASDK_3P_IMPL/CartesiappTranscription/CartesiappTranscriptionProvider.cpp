#include "CartesiappTranscriptionProvider.h"

#include <cartesiapp/transport/curl_http_transport.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace {

std::string resolveEncoding(va::audio::AudioFormat format)
{
    switch (format) {
    case va::audio::AudioFormat::FLOAT:
        return cartesiapp::stt::encoding::PCM_F32LE;
    case va::audio::AudioFormat::SHORT:
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::INT:
    case va::audio::AudioFormat::UNKNOWN:
    default:
        return cartesiapp::stt::encoding::PCM_S16LE;
    }
}

bool needsPcm16Conversion(va::audio::AudioFormat format)
{
    return format == va::audio::AudioFormat::BYTE ||
           format == va::audio::AudioFormat::INT ||
           format == va::audio::AudioFormat::UNKNOWN;
}

std::vector<int16_t> toPcm16(const uint8_t* data, size_t size, va::audio::AudioFormat format)
{
    std::vector<int16_t> pcm16;
    if (data == nullptr || size == 0) {
        return pcm16;
    }

    switch (format) {
    case va::audio::AudioFormat::SHORT: {
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        pcm16.assign(samples, samples + (size / sizeof(int16_t)));
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        const auto* samples = reinterpret_cast<const float*>(data);
        const size_t count = size / sizeof(float);
        pcm16.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const float clamped = std::clamp(samples[index], -1.0f, 1.0f);
            pcm16.push_back(static_cast<int16_t>(clamped * 32767.0f));
        }
        break;
    }
    case va::audio::AudioFormat::INT: {
        const auto* samples = reinterpret_cast<const int32_t*>(data);
        const size_t count = size / sizeof(int32_t);
        pcm16.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const int32_t scaled = samples[index] / 65536;
            pcm16.push_back(static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767)));
        }
        break;
    }
    case va::audio::AudioFormat::BYTE:
    default: {
        pcm16.reserve(size);
        for (size_t index = 0; index < size; ++index) {
            const int sample = (static_cast<int>(data[index]) - 128) << 8;
            pcm16.push_back(static_cast<int16_t>(std::clamp(sample, -32768, 32767)));
        }
        break;
    }
    }

    return pcm16;
}

std::shared_ptr<cartesiapp::transport::IHttpTransport> makeHttpTransport(const std::string& caFilePath)
{
    if (caFilePath.empty()) {
        return nullptr;
    }
    return std::make_shared<cartesiapp::transport::CurlHttpTransport>(caFilePath);
}

} // namespace

namespace va {
namespace thirdparty {

CartesiappTranscriptionProvider::CartesiappTranscriptionProvider(
    const std::string& apiKey,
    const std::string& model,
    const std::string& language,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _model(model),
      _language(language),
      _caFilePath(caFilePath)
{
}

CartesiappTranscriptionProvider::~CartesiappTranscriptionProvider()
{
    stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

bool CartesiappTranscriptionProvider::initializeTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        return true;
    }

    _client = std::make_unique<cartesiapp::SttRealtimeClient>(
        _apiKey,
        cartesiapp::api_versions::LATEST,
        nullptr,
        std::string(cartesiapp::CARTESIA_WS_BASE_URL) + "/stt/websocket",
        _caFilePath);
    connectCallbacks();
    return true;
}

void CartesiappTranscriptionProvider::releaseTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    _client.reset();
}

bool CartesiappTranscriptionProvider::startTranscriptionProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[CartesiaSTT] CARTESIA_API_KEY is empty; cannot start provider");
        return false;
    }

    if (!initializeTranscriptionProvider()) {
        return false;
    }

    if (needsPcm16Conversion(_audioConfig.format)) {
        spdlog::warn("[CartesiaSTT] Audio format {} is not directly supported by Cartesia; "
                     "audio will be converted to PCM16 before sending.",
                     static_cast<int>(_audioConfig.format));
    }

    cartesiapp::SttRealtimeConfig config;
    config.model = _model.empty() ? cartesiapp::stt::models::INK_WHISPER : _model;
    config.encoding = resolveEncoding(_audioConfig.format);
    config.sample_rate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    if (!_language.empty()) {
        config.language = _language;
    }

    try {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return false;
        }
        _client->connect(config);
    }
    catch (const std::exception& e) {
        spdlog::error("[CartesiaSTT] Failed to connect to the Cartesia STT API: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
        return false;
    }

    _stopNotified.store(false);
    _isRunning.store(true);

    if (_listener) {
        _listener->onTranscriptionStarted();
    }

    return true;
}

void CartesiappTranscriptionProvider::stopTranscriptionProvider()
{
    const bool wasRunning = _isRunning.exchange(false);

    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client) {
            try {
                _client->sendClose();
                _client->close();
            }
            catch (...) {
            }
        }
    }

    if (wasRunning) {
        notifyStoppedOnce();
    }
}

bool CartesiappTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning.load();
}

size_t CartesiappTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_isRunning.load() || data == nullptr || size == 0) {
        return 0;
    }

    std::vector<int16_t> converted;
    const uint8_t* audioBytes = data;
    size_t audioSize = size;

    if (needsPcm16Conversion(_audioConfig.format)) {
        converted = toPcm16(data, size, _audioConfig.format);
        if (converted.empty()) {
            return 0;
        }
        audioBytes = reinterpret_cast<const uint8_t*>(converted.data());
        audioSize = converted.size() * sizeof(int16_t);
    }

    try {
        std::vector<uint8_t> chunk(audioBytes, audioBytes + audioSize);
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return 0;
        }
        _client->sendAudio(chunk);
        return size;
    }
    catch (const std::exception& e) {
        spdlog::error("[CartesiaSTT] sendAudio failed: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
    }

    return 0;
}

va::audio::TranscriptionResult CartesiappTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult result;

    if (data == nullptr || size == 0) {
        return result;
    }
    if (_apiKey.empty()) {
        spdlog::error("[CartesiaSTT] CARTESIA_API_KEY is empty; cannot transcribe full segment");
        return result;
    }

    // Always normalize to PCM16 for the batch path: it keeps the headerless-PCM
    // encoding/sample_rate hints unambiguous regardless of the caller's native
    // sample format.
    auto pcm16 = toPcm16(data, size, _audioConfig.format);
    if (pcm16.empty()) {
        spdlog::warn("[CartesiaSTT] Full-segment transcription received empty PCM payload");
        return result;
    }

    std::vector<uint8_t> buffer(
        reinterpret_cast<const uint8_t*>(pcm16.data()),
        reinterpret_cast<const uint8_t*>(pcm16.data()) + pcm16.size() * sizeof(int16_t));

    cartesiapp::SttBatchRequest request;
    request.model = _model.empty() ? cartesiapp::stt::models::INK_WHISPER : _model;
    if (!_language.empty()) {
        request.language = _language;
    }
    request.encoding = cartesiapp::stt::encoding::PCM_S16LE;
    request.sample_rate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;

    try {
        cartesiapp::SttRestClient restClient(_apiKey, cartesiapp::api_versions::LATEST, makeHttpTransport(_caFilePath));
        auto response = restClient.transcribeBytesTyped(buffer, "audio.raw", request);
        result = buildResult(response);
    }
    catch (const std::exception& e) {
        spdlog::error("[CartesiaSTT] Full-segment transcription failed: {}", e.what());
    }

    return result;
}

void CartesiappTranscriptionProvider::commitEndOfSpeech()
{
    if (!_isRunning.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        try {
            _client->sendFinalize();
        }
        catch (const std::exception& e) {
            spdlog::warn("[CartesiaSTT] sendFinalize failed: {}", e.what());
        }
    }
}

void CartesiappTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* CartesiappTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void CartesiappTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig CartesiappTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void CartesiappTranscriptionProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnTranscript([this](const cartesiapp::SttTranscriptEvent& event) {
        if (!_listener) {
            return;
        }
        auto out = buildResult(event);
        if (out.text.empty()) {
            return;
        }
        if (event.is_final) {
            _listener->onFinalTranscription(out);
        }
        else {
            _listener->onPartialTranscription(out);
        }
    });

    _client->setOnError([this](const cartesiapp::SttErrorEvent& event) {
        if (_listener) {
            _listener->onTranscriptionError(event.message.empty() ? event.title : event.message);
        }
    });

    _client->setOnTransportError([this](const std::string& error) {
        if (_listener) {
            _listener->onTranscriptionError(error);
        }
    });

    _client->setOnClosed([this] {
        _isRunning.store(false);
        notifyStoppedOnce();
    });
}

void CartesiappTranscriptionProvider::notifyStoppedOnce()
{
    if (!_stopNotified.exchange(true) && _listener) {
        _listener->onTranscriptionStopped();
    }
}

va::audio::TranscriptionResult CartesiappTranscriptionProvider::buildResult(
    const cartesiapp::SttTranscriptEvent& event)
{
    va::audio::TranscriptionResult result;
    result.text = event.text;

    for (const auto& word : event.words) {
        va::audio::TranscriptionResult::Word w;
        w.text = word.word;
        w.startTimeMs = static_cast<int64_t>(word.start * 1000.0);
        w.endTimeMs = static_cast<int64_t>(word.end * 1000.0);
        result.words.push_back(std::move(w));
    }

    if (!result.words.empty()) {
        result.startTimeMs = result.words.front().startTimeMs;
        result.endTimeMs = result.words.back().endTimeMs;
    }
    else if (event.duration.has_value()) {
        result.endTimeMs = static_cast<int64_t>(*event.duration * 1000.0);
    }

    return result;
}

va::audio::TranscriptionResult CartesiappTranscriptionProvider::buildResult(
    const cartesiapp::SttTranscriptionResult& batch)
{
    va::audio::TranscriptionResult result;
    result.text = batch.text;

    const bool hasLanguage = !batch.language.empty();
    if (hasLanguage) {
        result.languageTags.push_back({ batch.language, 1.0f });
    }

    for (const auto& word : batch.words) {
        va::audio::TranscriptionResult::Word w;
        w.text = word.word;
        w.startTimeMs = static_cast<int64_t>(word.start * 1000.0);
        w.endTimeMs = static_cast<int64_t>(word.end * 1000.0);
        if (hasLanguage) {
            w.languageTags.push_back({ batch.language, 1.0f });
        }
        result.words.push_back(std::move(w));
    }

    result.endTimeMs = static_cast<int64_t>(batch.duration * 1000.0);
    if (!result.words.empty()) {
        result.startTimeMs = result.words.front().startTimeMs;
    }

    return result;
}

} // namespace thirdparty
} // namespace va
