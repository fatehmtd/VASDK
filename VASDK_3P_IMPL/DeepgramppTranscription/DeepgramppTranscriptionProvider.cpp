#include "DeepgramppTranscriptionProvider.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace {

std::string resolveEncoding(va::audio::AudioFormat format)
{
    switch (format) {
    case va::audio::AudioFormat::FLOAT:
        return deepgram::listen::encoding::LINEAR_32;
    case va::audio::AudioFormat::SHORT:
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::INT:
    case va::audio::AudioFormat::UNKNOWN:
    default:
        return deepgram::listen::encoding::LINEAR_16;
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

} // namespace

namespace va {
namespace thirdparty {

DeepgramppTranscriptionProvider::DeepgramppTranscriptionProvider(
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

DeepgramppTranscriptionProvider::~DeepgramppTranscriptionProvider()
{
    stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

bool DeepgramppTranscriptionProvider::initializeTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        return true;
    }

    _client = std::make_unique<deepgram::listen::ListenWebsocketClient>(_apiKey, nullptr, _caFilePath);
    connectCallbacks();
    return true;
}

void DeepgramppTranscriptionProvider::releaseTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    _client.reset();
}

bool DeepgramppTranscriptionProvider::startTranscriptionProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[DeepgramSTT] DEEPGRAM_API_KEY is empty; cannot start provider");
        return false;
    }

    if (!initializeTranscriptionProvider()) {
        return false;
    }

    if (needsPcm16Conversion(_audioConfig.format)) {
        spdlog::warn("[DeepgramSTT] Audio format {} is not directly supported by Deepgram; "
                     "audio will be converted to PCM16 before sending.",
                     static_cast<int>(_audioConfig.format));
    }

    deepgram::listen::LiveTranscriptionOptions options;
    options.model = _model.empty() ? deepgram::listen::models::nova_3::GENERAL : _model;
    options.language = _language.empty() ? deepgram::listen::languages::nova_3::MULTILINGUAL : _language;
    options.sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    options.channels = _audioConfig.channels > 0 ? _audioConfig.channels : 1;
    options.encoding = resolveEncoding(_audioConfig.format);
    options.interimResults = true;
    options.smartFormat = true;
    options.endpointing = 300;
    options.utteranceEndMs = 1000;

    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return false;
        }
        connected = _client->connect(options);
        if (connected) {
            _client->startKeepalive();
        }
    }

    if (!connected) {
        spdlog::error("[DeepgramSTT] Failed to connect to the Deepgram Listen API");
        if (_listener) {
            _listener->onTranscriptionError("Failed to connect to the Deepgram Listen API");
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

void DeepgramppTranscriptionProvider::stopTranscriptionProvider()
{
    const bool wasRunning = _isRunning.exchange(false);

    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client) {
            try {
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

bool DeepgramppTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning.load();
}

size_t DeepgramppTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
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
        if (_client && _client->streamAudio(chunk)) {
            return size;
        }
    }
    catch (const std::exception& e) {
        spdlog::error("[DeepgramSTT] streamAudio failed: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
    }

    return 0;
}

va::audio::TranscriptionResult DeepgramppTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult result;

    if (data == nullptr || size == 0) {
        return result;
    }
    if (_apiKey.empty()) {
        spdlog::error("[DeepgramSTT] DEEPGRAM_API_KEY is empty; cannot transcribe full segment");
        return result;
    }

    // Always normalize to PCM16 for the batch path: it keeps the REST content-type
    // unambiguous ("audio/L16") regardless of what the caller's native sample format is.
    auto pcm16 = toPcm16(data, size, _audioConfig.format);
    if (pcm16.empty()) {
        spdlog::warn("[DeepgramSTT] Full-segment transcription received empty PCM payload");
        return result;
    }

    std::vector<uint8_t> buffer(
        reinterpret_cast<const uint8_t*>(pcm16.data()),
        reinterpret_cast<const uint8_t*>(pcm16.data()) + pcm16.size() * sizeof(int16_t));

    deepgram::listen::LiveTranscriptionOptions options;
    options.model = _model.empty() ? deepgram::listen::models::nova_3::GENERAL : _model;
    options.language = _language.empty() ? deepgram::listen::languages::nova_3::MULTILINGUAL : _language;
    options.sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    options.channels = _audioConfig.channels > 0 ? _audioConfig.channels : 1;
    options.encoding = deepgram::listen::encoding::LINEAR_16;
    options.smartFormat = true;

    try {
        deepgram::listen::ListenRestClient restClient(_apiKey, nullptr, _caFilePath);
        auto response = restClient.transcribeBuffer(buffer, "audio/L16", options);
        if (!response) {
            spdlog::error("[DeepgramSTT] Full-segment transcription failed ({}): {}",
                          response.statusCode, response.errorMessage);
            return result;
        }
        if (!response.response.channels.empty()) {
            result = buildResult(response.response.channels.front(), std::nullopt, response.response.metadata.duration);
        }
    }
    catch (const std::exception& e) {
        spdlog::error("[DeepgramSTT] Full-segment transcription failed: {}", e.what());
    }

    return result;
}

void DeepgramppTranscriptionProvider::commitEndOfSpeech()
{
    if (!_isRunning.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        try {
            _client->sendFinalizeMessage();
        }
        catch (const std::exception& e) {
            spdlog::warn("[DeepgramSTT] sendFinalizeMessage failed: {}", e.what());
        }
    }
}

void DeepgramppTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* DeepgramppTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void DeepgramppTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig DeepgramppTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void DeepgramppTranscriptionProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnPartialTranscription([this](const deepgram::listen::TranscriptionResult& result) {
        if (!_listener) {
            return;
        }
        auto out = buildResult(result.channel, result.start, result.duration);
        if (!out.text.empty()) {
            _listener->onPartialTranscription(out);
        }
    });

    _client->setOnFinalTranscription([this](const deepgram::listen::TranscriptionResult& result) {
        if (!_listener) {
            return;
        }
        auto out = buildResult(result.channel, result.start, result.duration);
        if (!out.text.empty()) {
            _listener->onFinalTranscription(out);
        }
    });

    _client->setOnError([this](const std::string& error) {
        if (_listener) {
            _listener->onTranscriptionError(error);
        }
    });
}

void DeepgramppTranscriptionProvider::notifyStoppedOnce()
{
    if (!_stopNotified.exchange(true) && _listener) {
        _listener->onTranscriptionStopped();
    }
}

va::audio::TranscriptionResult DeepgramppTranscriptionProvider::buildResult(
    const deepgram::listen::Channel& channel,
    const std::optional<double>& start,
    const std::optional<double>& duration)
{
    va::audio::TranscriptionResult result;

    if (channel.alternatives.empty()) {
        return result;
    }

    const auto& alt = channel.alternatives.front();
    result.text = alt.transcript;
    result.confidence = static_cast<float>(alt.confidence);

    const bool hasLanguage = alt.language.has_value() && !alt.language->empty();
    if (hasLanguage) {
        result.languageTags.push_back({ *alt.language, 1.0f });
    }

    for (const auto& word : alt.words) {
        va::audio::TranscriptionResult::Word w;
        w.text = word.punctuated_word.has_value() ? *word.punctuated_word : word.word;
        w.startTimeMs = static_cast<int64_t>(word.start * 1000.0);
        w.endTimeMs = static_cast<int64_t>(word.end * 1000.0);
        w.confidence = static_cast<float>(word.confidence);
        if (hasLanguage) {
            w.languageTags.push_back({ *alt.language, 1.0f });
        }
        result.words.push_back(std::move(w));
    }

    if (start.has_value()) {
        result.startTimeMs = static_cast<int64_t>(*start * 1000.0);
        result.endTimeMs = result.startTimeMs + static_cast<int64_t>(duration.value_or(0.0) * 1000.0);
    }
    else if (!result.words.empty()) {
        result.startTimeMs = result.words.front().startTimeMs;
        result.endTimeMs = result.words.back().endTimeMs;
    }

    return result;
}

} // namespace thirdparty
} // namespace va
