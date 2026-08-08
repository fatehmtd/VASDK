#include "GradiumppTranscriptionProvider.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

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

// Wraps raw PCM16 mono audio in a canonical 44-byte WAV header. The Gradium
// batch ASR endpoint only accepts "wav", "pcm" or "opus" for input_format,
// and unlike the WebSocket setup message it has no sample_rate field of its
// own for "pcm" (which implies a fixed 24 kHz) — WAV carries its own sample
// rate so it works regardless of the caller's actual audio configuration.
std::vector<uint8_t> wrapPcm16AsWav(const int16_t* samples, size_t sampleCount, int sampleRate)
{
    const uint32_t dataSize = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * sizeof(int16_t);

    std::vector<uint8_t> wav(44 + dataSize);
    auto put = [&](size_t offset, const char* tag) { std::memcpy(wav.data() + offset, tag, 4); };
    auto putU32 = [&](size_t offset, uint32_t value) { std::memcpy(wav.data() + offset, &value, 4); };
    auto putU16 = [&](size_t offset, uint16_t value) { std::memcpy(wav.data() + offset, &value, 2); };

    put(0, "RIFF");
    putU32(4, static_cast<uint32_t>(36 + dataSize));
    put(8, "WAVE");
    put(12, "fmt ");
    putU32(16, 16);
    putU16(20, 1); // PCM
    putU16(22, 1); // mono
    putU32(24, static_cast<uint32_t>(sampleRate));
    putU32(28, byteRate);
    putU16(32, static_cast<uint16_t>(sizeof(int16_t)));
    putU16(34, 16); // bits per sample
    put(36, "data");
    putU32(40, dataSize);
    if (dataSize > 0) {
        std::memcpy(wav.data() + 44, samples, dataSize);
    }

    return wav;
}

} // namespace

namespace va {
namespace thirdparty {

GradiumppTranscriptionProvider::GradiumppTranscriptionProvider(
    const std::string& apiKey,
    const std::string& model,
    const std::string& language)
    : _apiKey(apiKey),
      _model(model),
      _language(language)
{
}

GradiumppTranscriptionProvider::~GradiumppTranscriptionProvider()
{
    stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

bool GradiumppTranscriptionProvider::initializeTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        return true;
    }

    _client = std::make_unique<gradium::AsrRealtimeClient>();
    connectCallbacks();
    return true;
}

void GradiumppTranscriptionProvider::releaseTranscriptionProvider()
{
    std::lock_guard<std::mutex> lock(_clientMutex);
    _client.reset();
}

std::string GradiumppTranscriptionProvider::resolveInputFormat() const
{
    switch (_audioConfig.sampleRate) {
    case 8000: return gradium::asr::input_formats::pcm_8000;
    case 16000: return gradium::asr::input_formats::pcm_16000;
    case 22050: return gradium::asr::input_formats::pcm_22050;
    case 24000: return gradium::asr::input_formats::pcm_24000;
    case 44100: return gradium::asr::input_formats::pcm_44100;
    case 48000: return gradium::asr::input_formats::pcm_48000;
    default: return gradium::asr::input_formats::pcm; // 24 kHz default
    }
}

bool GradiumppTranscriptionProvider::startTranscriptionProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[GradiumSTT] GRADIUM_API_KEY is empty; cannot start provider");
        return false;
    }

    if (!initializeTranscriptionProvider()) {
        return false;
    }

    if (needsPcm16Conversion(_audioConfig.format)) {
        spdlog::warn("[GradiumSTT] Audio format {} is not directly supported by Gradium; "
                     "audio will be converted to PCM16 before sending.",
                     static_cast<int>(_audioConfig.format));
    }

    gradium::AsrRealtimeSetup setup;
    setup.input_format = resolveInputFormat();
    setup.model_name = _model.empty() ? gradium::asr::models::default_model : _model;
    if (!_language.empty()) {
        setup.json_config.language = _language;
    }

    _ready.store(false);
    _setupFailed.store(false);
    _stopNotified.store(false);

    try {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (!_client) {
            return false;
        }
        _client->connect(_apiKey, setup);
    }
    catch (const std::exception& e) {
        spdlog::error("[GradiumSTT] Failed to connect to the Gradium ASR API: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(_readyMutex);
        const bool becameReady = _readyCv.wait_for(lock, std::chrono::seconds(15), [this] {
            return _ready.load() || _setupFailed.load();
        });
        if (!becameReady) {
            spdlog::error("[GradiumSTT] Timed out waiting for the ASR session to become ready");
            if (_listener) {
                _listener->onTranscriptionError("Timed out waiting for the ASR session to become ready");
            }
            return false;
        }
    }

    if (_setupFailed.load()) {
        return false;
    }

    _isRunning.store(true);

    if (_listener) {
        _listener->onTranscriptionStarted();
    }

    return true;
}

void GradiumppTranscriptionProvider::stopTranscriptionProvider()
{
    const bool wasRunning = _isRunning.exchange(false);

    {
        std::lock_guard<std::mutex> lock(_clientMutex);
        if (_client) {
            try {
                _client->sendEndOfStream();
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

bool GradiumppTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning.load();
}

size_t GradiumppTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
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
        spdlog::error("[GradiumSTT] sendAudio failed: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
    }

    return 0;
}

va::audio::TranscriptionResult GradiumppTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult result;

    if (data == nullptr || size == 0) {
        return result;
    }
    if (_apiKey.empty()) {
        spdlog::error("[GradiumSTT] GRADIUM_API_KEY is empty; cannot transcribe full segment");
        return result;
    }

    auto pcm16 = toPcm16(data, size, _audioConfig.format);
    if (pcm16.empty()) {
        spdlog::warn("[GradiumSTT] Full-segment transcription received empty PCM payload");
        return result;
    }

    const int sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    const auto wavBytes = wrapPcm16AsWav(pcm16.data(), pcm16.size(), sampleRate);

    gradium::AsrConfig config;
    config.input_format = gradium::asr::input_formats::wav;
    config.model_name = _model.empty() ? gradium::asr::models::default_model : _model;
    if (!_language.empty()) {
        config.json_config.language = _language;
    }

    try {
        gradium::AsrRestClient restClient(_apiKey);
        const std::string ndjson = restClient.transcribe(config, wavBytes);
        result = parseNdjsonResult(ndjson);
    }
    catch (const std::exception& e) {
        spdlog::error("[GradiumSTT] Full-segment transcription failed: {}", e.what());
    }

    return result;
}

void GradiumppTranscriptionProvider::commitEndOfSpeech()
{
    if (!_isRunning.load()) {
        return;
    }

    const int flushId = _flushCounter.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingFlushId = flushId;
    }

    std::lock_guard<std::mutex> lock(_clientMutex);
    if (_client) {
        try {
            _client->sendFlush(flushId);
        }
        catch (const std::exception& e) {
            spdlog::warn("[GradiumSTT] sendFlush failed: {}", e.what());
        }
    }
}

void GradiumppTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* GradiumppTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void GradiumppTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig GradiumppTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void GradiumppTranscriptionProvider::connectCallbacks()
{
    if (!_client) {
        return;
    }

    _client->setOnParsedMessage([this](const gradium::AsrRealtimeMessage& message) {
        switch (message.type) {
        case gradium::AsrRealtimeMessage::Type::Ready: {
            {
                std::lock_guard<std::mutex> lock(_readyMutex);
                _ready.store(true);
            }
            _readyCv.notify_all();
            break;
        }
        case gradium::AsrRealtimeMessage::Type::Text: {
            spdlog::debug("[GradiumSTT] partial: \"{}\"", message.segment.text);
            va::audio::TranscriptionResult combinedResult;
            bool shouldFirePartial = false;
            {
                std::lock_guard<std::mutex> lock(_pendingMutex);
                if (!message.segment.text.empty()) {
                    _lastPartialSegment = message.segment;
                    _hasPendingPartial = true;
                }
                std::string combinedText = _accumulatedText;
                if (!message.segment.text.empty()) {
                    if (!combinedText.empty()) combinedText += ' ';
                    combinedText += message.segment.text;
                }
                if (!combinedText.empty()) {
                    gradium::TranscriptSegment combinedSegment = message.segment;
                    combinedSegment.text = combinedText;
                    combinedResult = buildResult(combinedSegment);
                    shouldFirePartial = true;
                }
            }
            if (shouldFirePartial && _listener) {
                _listener->onPartialTranscription(combinedResult);
            }
            break;
        }
        case gradium::AsrRealtimeMessage::Type::EndText: {
            spdlog::debug("[GradiumSTT] end_text: \"{}\"", message.segment.text);
            std::lock_guard<std::mutex> lock(_pendingMutex);
            // segment.text is usually empty here; the content came via the
            // preceding partial. Never touches _pendingFlushId - only the
            // matching Flushed ack below clears it.
            std::string finalizedChunk = message.segment.text;
            if (finalizedChunk.empty() && _hasPendingPartial) {
                finalizedChunk = _lastPartialSegment.text;
            }
            if (!finalizedChunk.empty()) {
                if (!_accumulatedText.empty()) _accumulatedText += ' ';
                _accumulatedText += finalizedChunk;
            }
            _hasPendingPartial = false;
            break;
        }
        case gradium::AsrRealtimeMessage::Type::Flushed: {
            // sendFlush() only guarantees this ack, not a matching end_text.
            va::audio::TranscriptionResult finalResult;
            bool shouldFire = false;
            {
                std::lock_guard<std::mutex> lock(_pendingMutex);
                spdlog::debug("[GradiumSTT] flushed ack (flush_id={}, pending={}, accumulated=\"{}\", hasPartial={})",
                              message.flush_id, _pendingFlushId, _accumulatedText, _hasPendingPartial);
                if (message.flush_id == _pendingFlushId) {
                    std::string finalText = _accumulatedText;
                    gradium::TranscriptSegment seg = _hasPendingPartial ? _lastPartialSegment : gradium::TranscriptSegment{};
                    if (_hasPendingPartial && !_lastPartialSegment.text.empty()) {
                        if (!finalText.empty()) finalText += ' ';
                        finalText += _lastPartialSegment.text;
                    }
                    if (!finalText.empty()) {
                        seg.text = finalText;
                        finalResult = buildResult(seg);
                        shouldFire = true;
                    }
                    _accumulatedText.clear();
                    _hasPendingPartial = false;
                    _pendingFlushId = -1;
                }
            }
            if (shouldFire && _listener) {
                spdlog::info("[GradiumSTT] Turn finalized on flush (id={}): \"{}\"",
                             message.flush_id, finalResult.text);
                _listener->onFinalTranscription(finalResult);
            }
            break;
        }
        case gradium::AsrRealtimeMessage::Type::EndOfStream: {
            const bool wasRunning = _isRunning.exchange(false);
            if (wasRunning) {
                notifyStoppedOnce();
            }
            break;
        }
        case gradium::AsrRealtimeMessage::Type::Error: {
            {
                std::lock_guard<std::mutex> lock(_readyMutex);
                _setupFailed.store(true);
            }
            _readyCv.notify_all();
            if (_listener) {
                _listener->onTranscriptionError(message.error_message);
            }
            break;
        }
        case gradium::AsrRealtimeMessage::Type::Step:
        case gradium::AsrRealtimeMessage::Type::UNKNOWN:
        default:
            break;
        }
    });

    _client->setOnError([this](const std::string& error) {
        {
            std::lock_guard<std::mutex> lock(_readyMutex);
            _setupFailed.store(true);
        }
        _readyCv.notify_all();
        if (_listener) {
            _listener->onTranscriptionError(error);
        }
    });

    _client->setOnClosed([this] {
        {
            std::lock_guard<std::mutex> lock(_readyMutex);
            _setupFailed.store(true);
        }
        _readyCv.notify_all();
        const bool wasRunning = _isRunning.exchange(false);
        if (wasRunning) {
            notifyStoppedOnce();
        }
    });
}

void GradiumppTranscriptionProvider::notifyStoppedOnce()
{
    if (!_stopNotified.exchange(true) && _listener) {
        _listener->onTranscriptionStopped();
    }
}

va::audio::TranscriptionResult GradiumppTranscriptionProvider::buildResult(
    const gradium::TranscriptSegment& segment)
{
    va::audio::TranscriptionResult result;
    result.text = segment.text;
    result.startTimeMs = static_cast<int64_t>(segment.start_s * 1000.0);
    result.endTimeMs = static_cast<int64_t>(segment.stop_s * 1000.0);
    return result;
}

va::audio::TranscriptionResult GradiumppTranscriptionProvider::parseNdjsonResult(const std::string& ndjson)
{
    va::audio::TranscriptionResult result;

    std::ostringstream text;
    double firstStart = -1.0;
    double lastStop = 0.0;
    bool first = true;

    std::istringstream lines(ndjson);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        auto payload = nlohmann::json::parse(line, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            continue;
        }

        const std::string type = payload.value("type", std::string{});
        if (type != "text" && type != "end_text" && !payload.contains("text")) {
            continue;
        }

        const std::string segmentText = payload.value("text", std::string{});
        if (segmentText.empty()) {
            continue;
        }

        if (!first) {
            text << ' ';
        }
        text << segmentText;
        first = false;

        if (payload.contains("start_s") && payload["start_s"].is_number()) {
            const double startS = payload["start_s"].get<double>();
            if (firstStart < 0.0 || startS < firstStart) {
                firstStart = startS;
            }
        }
        if (payload.contains("stop_s") && payload["stop_s"].is_number()) {
            lastStop = std::max(lastStop, payload["stop_s"].get<double>());
        }
    }

    result.text = text.str();
    result.startTimeMs = firstStart > 0.0 ? static_cast<int64_t>(firstStart * 1000.0) : 0;
    result.endTimeMs = static_cast<int64_t>(lastStop * 1000.0);
    return result;
}

} // namespace thirdparty
} // namespace va
