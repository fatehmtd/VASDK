#include "FishaudioppTranscriptionProvider.h"

#include <fishaudiopp/transport/curl_http_transport.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>

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

// Fish Audio's ASR endpoint sniffs the audio format from the multipart upload's file
// name extension (fishaudiopp always sends an empty content_type for byte uploads,
// see AsrClient::transcribeBytes), so headerless PCM must be wrapped in a minimal WAV
// container and uploaded as "audio.wav" rather than sent as raw samples.
std::vector<uint8_t> buildWavBytes(const std::vector<int16_t>& pcm16, int sampleRate, int channels)
{
    const uint32_t dataSize = static_cast<uint32_t>(pcm16.size() * sizeof(int16_t));
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate * blockAlign);
    const uint32_t riffSize = 36 + dataSize;

    std::vector<uint8_t> wav;
    wav.reserve(44 + dataSize);

    auto putU32 = [&wav](uint32_t v) {
        wav.push_back(static_cast<uint8_t>(v & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto putU16 = [&wav](uint16_t v) {
        wav.push_back(static_cast<uint8_t>(v & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto putTag = [&wav](const char* tag) {
        wav.insert(wav.end(), tag, tag + 4);
    };

    putTag("RIFF");
    putU32(riffSize);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16); // PCM fmt chunk size
    putU16(1);  // PCM
    putU16(static_cast<uint16_t>(channels));
    putU32(static_cast<uint32_t>(sampleRate));
    putU32(byteRate);
    putU16(blockAlign);
    putU16(bitsPerSample);
    putTag("data");
    putU32(dataSize);

    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm16.data());
    wav.insert(wav.end(), bytes, bytes + dataSize);

    return wav;
}

std::shared_ptr<fishaudiopp::transport::HttpTransportInterface> makeHttpTransport(const std::string& caFilePath)
{
    if (caFilePath.empty()) {
        return nullptr;
    }
    return std::make_shared<fishaudiopp::transport::CurlHttpTransport>(caFilePath);
}

} // namespace

namespace va {
namespace thirdparty {

FishaudioppTranscriptionProvider::FishaudioppTranscriptionProvider(
    const std::string& apiKey,
    const std::string& language,
    const std::string& caFilePath)
    : _apiKey(apiKey),
      _language(language),
      _caFilePath(caFilePath)
{
}

FishaudioppTranscriptionProvider::~FishaudioppTranscriptionProvider()
{
    stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

bool FishaudioppTranscriptionProvider::initializeTranscriptionProvider()
{
    return true;
}

void FishaudioppTranscriptionProvider::releaseTranscriptionProvider()
{
}

bool FishaudioppTranscriptionProvider::startTranscriptionProvider()
{
    if (_isRunning.load()) {
        return true;
    }

    if (_apiKey.empty()) {
        spdlog::error("[FishAudioSTT] FISHAUDIO_API_KEY is empty; cannot start provider");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_bufferMutex);
        _buffer.clear();
    }

    _workerShouldExit = false;
    _worker = std::thread(&FishaudioppTranscriptionProvider::workerLoop, this);

    _isRunning.store(true);
    if (_listener) {
        _listener->onTranscriptionStarted();
    }
    return true;
}

void FishaudioppTranscriptionProvider::stopTranscriptionProvider()
{
    if (!_isRunning.exchange(false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_workerMutex);
        _workerShouldExit = true;
    }
    _workerCv.notify_one();
    if (_worker.joinable()) {
        _worker.join();
    }

    if (_listener) {
        _listener->onTranscriptionStopped();
    }
}

bool FishaudioppTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _isRunning.load();
}

size_t FishaudioppTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_isRunning.load() || data == nullptr || size == 0) {
        return 0;
    }

    auto pcm16 = toPcm16(data, size, _audioConfig.format);
    if (pcm16.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(_bufferMutex);
    _buffer.insert(_buffer.end(), pcm16.begin(), pcm16.end());
    return size;
}

va::audio::TranscriptionResult FishaudioppTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return {};
    }

    auto pcm16 = toPcm16(data, size, _audioConfig.format);
    if (pcm16.empty()) {
        spdlog::warn("[FishAudioSTT] Full-segment transcription received empty PCM payload");
        return {};
    }

    return transcribe(pcm16);
}

void FishaudioppTranscriptionProvider::commitEndOfSpeech()
{
    if (!_isRunning.load()) {
        return;
    }

    std::vector<int16_t> segment;
    {
        std::lock_guard<std::mutex> lock(_bufferMutex);
        segment.swap(_buffer);
    }

    if (segment.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_workerMutex);
        _pendingSegment = std::move(segment);
        _hasPendingSegment = true;
    }
    _workerCv.notify_one();
}

void FishaudioppTranscriptionProvider::clearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(_bufferMutex);
    _buffer.clear();
}

void FishaudioppTranscriptionProvider::setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* FishaudioppTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void FishaudioppTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
    if (needsPcm16Conversion(config.format)) {
        spdlog::warn("[FishAudioSTT] Audio format {} is not directly supported by Fish Audio; "
                     "audio will be converted to PCM16 before sending.",
                     static_cast<int>(config.format));
    }
}

va::audio::AudioConfig FishaudioppTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void FishaudioppTranscriptionProvider::workerLoop()
{
    for (;;) {
        std::vector<int16_t> segment;
        {
            std::unique_lock<std::mutex> lock(_workerMutex);
            _workerCv.wait(lock, [this] { return _workerShouldExit || _hasPendingSegment; });
            if (_workerShouldExit && !_hasPendingSegment) {
                return;
            }
            segment = std::move(_pendingSegment);
            _pendingSegment.clear();
            _hasPendingSegment = false;
        }

        auto result = transcribe(segment);
        if (_listener) {
            _listener->onFinalTranscription(result);
        }
    }
}

va::audio::TranscriptionResult FishaudioppTranscriptionProvider::transcribe(const std::vector<int16_t>& pcm16)
{
    va::audio::TranscriptionResult result;
    if (pcm16.empty()) {
        return result;
    }
    if (_apiKey.empty()) {
        spdlog::error("[FishAudioSTT] FISHAUDIO_API_KEY is empty; cannot transcribe segment");
        return result;
    }

    const int sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    const int channels = _audioConfig.channels > 0 ? _audioConfig.channels : 1;
    auto wavBytes = buildWavBytes(pcm16, sampleRate, channels);

    fishaudiopp::asr::AsrRequest request;
    if (!_language.empty()) {
        request.language = _language;
    }
    request.ignore_timestamps = false;

    try {
        fishaudiopp::asr::AsrClient client(_apiKey, makeHttpTransport(_caFilePath));
        auto response = client.transcribeBytesTyped(wavBytes, "audio.wav", request);
        result.text = response.text;
        result.endTimeMs = static_cast<int64_t>(response.duration * 1000.0);
    }
    catch (const std::exception& e) {
        spdlog::error("[FishAudioSTT] Transcription failed: {}", e.what());
        if (_listener) {
            _listener->onTranscriptionError(e.what());
        }
    }

    return result;
}

} // namespace thirdparty
} // namespace va
