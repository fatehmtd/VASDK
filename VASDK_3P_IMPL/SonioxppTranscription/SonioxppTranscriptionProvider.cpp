#include "SonioxppTranscriptionProvider.h"

#include <sonioxpp/api_constants.hpp>
#include <sonioxpp/stt_client.hpp>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

using json = nlohmann::json;

std::string resolveRealtimeAudioFormat(va::audio::AudioFormat format)
{
    switch (format) {
    case va::audio::AudioFormat::SHORT:
        return soniox::stt::audio_formats::pcm_s16le;
    case va::audio::AudioFormat::INT:
        return soniox::stt::audio_formats::pcm_s32le;
    case va::audio::AudioFormat::FLOAT:
        return soniox::stt::audio_formats::pcm_f32le;
    case va::audio::AudioFormat::BYTE:
    case va::audio::AudioFormat::UNKNOWN:
    default:
        return soniox::stt::audio_formats::pcm_s16le;
    }
}

bool needsPcm16Conversion(va::audio::AudioFormat format)
{
    return format == va::audio::AudioFormat::BYTE || format == va::audio::AudioFormat::UNKNOWN;
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

std::vector<soniox::Token> parseTokensFromMessage(
    const std::string& rawMessage,
    bool& finished,
    bool& sawManualFinalize)
{
    finished = false;
    sawManualFinalize = false;

    json payload;
    try {
        payload = json::parse(rawMessage);
    }
    catch (const json::exception&) {
        return {};
    }

    finished = payload.value("finished", false);
    if (!payload.contains("tokens") || !payload["tokens"].is_array()) {
        return {};
    }

    std::vector<soniox::Token> tokens;
    for (const auto& item : payload["tokens"]) {
        soniox::Token token;
        token.text = item.value("text", std::string());
        token.is_final = item.value("is_final", false);
        token.start_ms = item.value("start_ms", 0);
        token.end_ms = item.value("end_ms", 0);
        token.duration_ms = (token.end_ms >= token.start_ms) ? (token.end_ms - token.start_ms) : 0;
        token.confidence = item.value("confidence", 0.0);

        const auto speakerIt = item.find("speaker");
        if (speakerIt != item.end()) {
            if (speakerIt->is_number_integer()) {
                token.speaker = speakerIt->get<int>();
            }
            else if (speakerIt->is_string()) {
                try {
                    token.speaker = std::stoi(speakerIt->get<std::string>());
                }
                catch (...) {
                }
            }
        }

        const auto langIt = item.find("language");
        if (langIt != item.end() && !langIt->is_null()) {
            token.language = langIt->get<std::string>();
        }

        if (token.text == "<fin>") {
            sawManualFinalize = true;
        }

        tokens.push_back(std::move(token));
    }

    return tokens;
}

} // namespace

namespace va {
namespace thirdparty {

SonioxppTranscriptionProvider::SonioxppTranscriptionProvider(const std::string& apiKey, const std::string& caFilePath)
    : _apiKey(apiKey), _caFilePath(caFilePath)
{
}

SonioxppTranscriptionProvider::~SonioxppTranscriptionProvider()
{
    stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

bool SonioxppTranscriptionProvider::initializeTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        return true;
    }

    _client = std::make_unique<soniox::SttRealtimeClient>(
        nullptr, "wss://stt-rt.soniox.com/transcribe-websocket", _caFilePath);
    connectCallbacks();
    return true;
}

void SonioxppTranscriptionProvider::releaseTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    _client.reset();
}

bool SonioxppTranscriptionProvider::startTranscriptionProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[SonioxSTT] SONIOX_API_KEY is empty; cannot start provider");
        return false;
    }

    if (!initializeTranscriptionProvider()) {
        return false;
    }

    _stopNotified.store(false);
    _isRunning.store(true);

    return true;
}

void SonioxppTranscriptionProvider::stopTranscriptionProvider()
{
    const bool wasRunning = _isRunning.exchange(false);

    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client && _sessionStarted.load()) {
            try {
                _client->sendEndOfAudio();
            }
            catch (...) {
            }
            try {
                _client->close();
            }
            catch (...) {
            }
        }
    }

    _sessionStarted.store(false);

    if (wasRunning) {
        notifyStoppedOnce();
    }
}

bool SonioxppTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning.load();
}

size_t SonioxppTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_isRunning.load() || data == nullptr || size == 0) {
        return 0;
    }

    if (!ensureRealtimeSessionStarted()) {
        return 0;
    }

    try {
        std::vector<uint8_t> chunk(data, data + size);
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client) {
            _client->sendAudio(chunk);
            return size;
        }
    }
    catch (const std::exception& e) {
        spdlog::error("[SonioxSTT] sendAudio failed: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
    }

    return 0;
}

va::audio::TranscriptionResult SonioxppTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult result;

    if (data == nullptr || size == 0) {
        return result;
    }
    if (_apiKey.empty()) {
        spdlog::error("[SonioxSTT] SONIOX_API_KEY is empty; cannot transcribe full segment");
        return result;
    }

    const int sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    const int channels = _audioConfig.channels > 0 ? _audioConfig.channels : 1;
    const bool convertToPcm16 = needsPcm16Conversion(_audioConfig.format);

    std::vector<int16_t> convertedPcm16;
    const uint8_t* audioBytes = data;
    size_t audioSize = size;
    std::string audioFormat = resolveRealtimeAudioFormat(_audioConfig.format);

    if (convertToPcm16) {
        convertedPcm16 = toPcm16(data, size, _audioConfig.format);
        if (convertedPcm16.empty()) {
            spdlog::warn("[SonioxSTT] Full-segment transcription received empty PCM payload");
            return result;
        }

        audioBytes = reinterpret_cast<const uint8_t*>(convertedPcm16.data());
        audioSize = convertedPcm16.size() * sizeof(int16_t);
        audioFormat = soniox::stt::audio_formats::pcm_s16le;
    }

    soniox::SttRealtimeConfig cfg;
    cfg.api_key = _apiKey;
    cfg.model = soniox::stt::models::realtime_v5;
    cfg.audio_format = audioFormat;
    cfg.sample_rate = sampleRate;
    cfg.num_channels = channels;
    cfg.enable_endpoint_detection = false;

    std::vector<soniox::Token> finalTokens;
    std::string errorMessage;
    std::mutex callbackMutex;
    std::condition_variable doneCv;
    bool finished = false;

    try {
        soniox::SttRealtimeClient client(nullptr, "wss://stt-rt.soniox.com/transcribe-websocket", _caFilePath);
        client.setOnMessage([&](const std::string& rawMessage) {
            bool messageFinished = false;
            bool sawManualFinalize = false;
            auto tokens = parseTokensFromMessage(rawMessage, messageFinished, sawManualFinalize);
            std::lock_guard<std::mutex> lock(callbackMutex);

            for (const auto& token : tokens) {
                if (token.is_final) {
                    finalTokens.push_back(token);
                }
            }

            if (messageFinished || sawManualFinalize) {
                finished = true;
                doneCv.notify_all();
            }
        });
        client.setOnError([&](const std::string& err) {
            std::lock_guard<std::mutex> lock(callbackMutex);
            errorMessage = err;
            finished = true;
            doneCv.notify_all();
        });
        client.setOnClosed([&]() {
            std::lock_guard<std::mutex> lock(callbackMutex);
            finished = true;
            doneCv.notify_all();
        });

        client.connect(cfg);

        std::thread sender([&client, audioBytes, audioSize]() {
            static constexpr size_t kChunkBytes = 4096;

            for (size_t offset = 0; offset < audioSize; offset += kChunkBytes) {
                const size_t chunkSize = std::min(kChunkBytes, audioSize - offset);
                std::vector<std::uint8_t> chunk(audioBytes + offset, audioBytes + offset + chunkSize);
                client.sendAudio(chunk);
            }
            client.sendManualFinalize();
            client.sendEndOfAudio();
        });

        {
            std::unique_lock<std::mutex> lock(callbackMutex);
            doneCv.wait(lock, [&]() { return finished; });
        }

        client.close();
        sender.join();

        if (!errorMessage.empty()) {
            spdlog::error("[SonioxSTT] Full-segment transcription failed: {}", errorMessage);
            return result;
        }

        result = buildResultFromTokens(finalTokens, true, false);
    }
    catch (const std::exception& e) {
        spdlog::error("[SonioxSTT] Full-segment transcription failed: {}", e.what());
    }

    return result;
}

void SonioxppTranscriptionProvider::commitEndOfSpeech()
{
    if (!_isRunning.load()) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client && _sessionStarted.load()) {
            _client->sendManualFinalize();
        }
    }
    catch (const std::exception& e) {
        spdlog::warn("[SonioxSTT] sendManualFinalize failed: {}", e.what());
    }
}

void SonioxppTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* SonioxppTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void SonioxppTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig SonioxppTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void SonioxppTranscriptionProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnMessage([this](const std::string& rawMessage) {
        bool messageFinished = false;
        bool sawManualFinalize = false;
        auto tokens = parseTokensFromMessage(rawMessage, messageFinished, sawManualFinalize);

        if (_listener != nullptr && !tokens.empty()) {
            const bool hasFinal = std::any_of(tokens.begin(), tokens.end(),
                [](const soniox::Token& token) { return token.is_final; });

            if (hasFinal) {
                auto finalResult = buildResultFromTokens(tokens, true, false);
                if (!finalResult.text.empty()) {
                    _listener->onFinalTranscription(finalResult);
                }

                auto partialResult = buildResultFromTokens(tokens, false, true);
                if (!partialResult.text.empty()) {
                    _listener->onPartialTranscription(partialResult);
                }
            }
            else {
                auto partialResult = buildResultFromTokens(tokens, true, true);
                if (!partialResult.text.empty()) {
                    _listener->onPartialTranscription(partialResult);
                }
            }
        }

        if (messageFinished || sawManualFinalize) {
            _sessionStarted.store(false);
            _isRunning.store(false);
            notifyStoppedOnce();
        }
    });

    _client->setOnClosed([this]() {
        _sessionStarted.store(false);
        _isRunning.store(false);
        notifyStoppedOnce();
    });

    _client->setOnError([this](const std::string& errorMessage) {
        if (_listener) {
            _listener->onTranscriptionError(errorMessage);
        }
    });
}

bool SonioxppTranscriptionProvider::ensureRealtimeSessionStarted()
{
    if (_sessionStarted.load()) {
        return true;
    }

    soniox::SttRealtimeConfig cfg;
    cfg.api_key = _apiKey;
    cfg.model = soniox::stt::models::realtime_v5;
    cfg.audio_format = resolveRealtimeAudioFormat(_audioConfig.format);
    cfg.sample_rate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    cfg.num_channels = _audioConfig.channels > 0 ? _audioConfig.channels : 1;
    cfg.enable_endpoint_detection = true;

    if (needsPcm16Conversion(_audioConfig.format)) {
        spdlog::warn("[SonioxSTT] Audio format {} is not directly supported by Soniox realtime STT; provider will require PCM16-compatible input.",
                     static_cast<int>(_audioConfig.format));
    }

    try {
        {
            std::lock_guard<std::mutex> lock(_clientMutex);
            if (!_client) {
                return false;
            }
            if (_sessionStarted.load()) {
                return true;
            }
            _client->connect(cfg);
            _sessionStarted.store(true);
        }

        if (_listener) {
            _listener->onTranscriptionStarted();
        }

        return true;
    }
    catch (const std::exception& e) {
        spdlog::error("[SonioxSTT] Failed to connect: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
        return false;
    }
}

void SonioxppTranscriptionProvider::notifyStoppedOnce()
{
    if (!_stopNotified.exchange(true) && _listener) {
        _listener->onTranscriptionStopped();
    }
}

va::audio::TranscriptionResult SonioxppTranscriptionProvider::buildResultFromTokens(
    const std::vector<soniox::Token>& tokens,
    bool includeFinal,
    bool includeNonFinal)
{
    va::audio::TranscriptionResult result;

    bool haveTiming = false;
    double confidenceSum = 0.0;
    int confidenceCount = 0;

    for (const auto& token : tokens) {
        const bool includeToken = (token.is_final && includeFinal) || (!token.is_final && includeNonFinal);
        if (!includeToken) {
            continue;
        }

        result.text += token.text;

        if (!token.language.empty()) {
            auto it = std::find_if(result.languageTags.begin(), result.languageTags.end(),
                                   [&token](const va::audio::TranscriptionResult::LanguageTag& tag) {
                                       return tag.code == token.language;
                                   });
            if (it == result.languageTags.end()) {
                result.languageTags.push_back({token.language, 1.0f});
            }
        }

        va::audio::TranscriptionResult::Word word;
        word.text = token.text;
        word.startTimeMs = token.start_ms;
        word.endTimeMs = token.end_ms;
        word.confidence = static_cast<float>(token.confidence);
        if (!token.language.empty()) {
            word.languageTags.push_back({token.language, 1.0f});
        }
        result.words.push_back(std::move(word));

        if (!haveTiming) {
            result.startTimeMs = token.start_ms;
            result.endTimeMs = token.end_ms;
            haveTiming = true;
        } else {
            result.startTimeMs = std::min(result.startTimeMs, static_cast<int64_t>(token.start_ms));
            result.endTimeMs = std::max(result.endTimeMs, static_cast<int64_t>(token.end_ms));
        }

        if (token.confidence > 0.0) {
            confidenceSum += token.confidence;
            confidenceCount++;
        }
    }

    if (confidenceCount > 0) {
        result.confidence = static_cast<float>(confidenceSum / static_cast<double>(confidenceCount));
    }

    return result;
}

} // namespace thirdparty
} // namespace va
