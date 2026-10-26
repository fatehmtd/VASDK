#include "WhistleTranscriptionProvider.hpp"
#include "impl/NeedleEngine.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {

constexpr const char* kDefaultModelFile = "whistle.cact";

std::string resolveModelPath(const std::string& modelPath)
{
    std::filesystem::path p(modelPath);
    if (p.extension() == ".cact") return modelPath;
    return (p / kDefaultModelFile).string();
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int64_t secondsToMs(double s)
{
    return static_cast<int64_t>(std::llround(s * 1000.0));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WhistleTranscriptionProvider::WhistleTranscriptionProvider(const std::string& modelPath,
                                                           const std::string& enginePath)
    : _modelPath(resolveModelPath(modelPath))
    , _enginePath(enginePath)
{
}

WhistleTranscriptionProvider::~WhistleTranscriptionProvider()
{
    if (_running) stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool WhistleTranscriptionProvider::initializeTranscriptionProvider()
{
    if (_initialized) {
        spdlog::warn("[WhistleSTT] Already initialized");
        return true;
    }
    if (!NeedleEngine::instance().load(_modelPath, _enginePath)) {
        spdlog::error("[WhistleSTT] Failed to load model from '{}'", _modelPath);
        return false;
    }
    _initialized = true;
    spdlog::info("[WhistleSTT] Initialized with model '{}'", _modelPath);
    return true;
}

void WhistleTranscriptionProvider::releaseTranscriptionProvider()
{
    // The engine has no unload; the shared model stays resident for other instances.
    _initialized = false;
}

bool WhistleTranscriptionProvider::startTranscriptionProvider()
{
    if (_running) return true;
    if (!_initialized) {
        spdlog::error("[WhistleSTT] Not initialized, cannot start");
        return false;
    }
    if (_audioConfig.sampleRate != 16000 || _audioConfig.channels != 1) {
        spdlog::warn("[WhistleSTT] Audio is {} Hz / {} ch; Whistle requires 16000 Hz mono",
                     _audioConfig.sampleRate, _audioConfig.channels);
    }

    _running = true;
    _hasNewAudio = false;
    _lastWriteTime = std::chrono::steady_clock::now();
    _thread = std::thread(&WhistleTranscriptionProvider::inferenceLoop, this);

    spdlog::info("[WhistleSTT] Started");
    return true;
}

void WhistleTranscriptionProvider::stopTranscriptionProvider()
{
    if (!_running) return;

    _running = false;
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();

    std::lock_guard<std::mutex> lock(_mtx);
    _buffer.clear();
    _hasNewAudio = false;
    _endOfSpeech = false;
    _bufferSizeAtEndOfSpeech = 0;
    _cachedResult = {};
    _lastInferSamples = 0;

    spdlog::info("[WhistleSTT] Stopped");
}

bool WhistleTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _running;
}

// ---------------------------------------------------------------------------
// Audio input
// ---------------------------------------------------------------------------

size_t WhistleTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_running || !data || size == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(_mtx);
        appendAudioAsFloat(data, size, _buffer);
        _lastWriteTime = std::chrono::steady_clock::now();
        _hasNewAudio = true;
    }
    _cv.notify_one();
    return size;
}

va::audio::TranscriptionResult WhistleTranscriptionProvider::writeFullAudioData(
    const uint8_t* data, size_t size)
{
    if (!_initialized) {
        spdlog::error("[WhistleSTT] writeFullAudioData called before initialization");
        return {};
    }
    if (!data || size == 0) return {};

    std::vector<float> audio;
    appendAudioAsFloat(data, size, audio);
    return runInference(audio);
}

void WhistleTranscriptionProvider::commitEndOfSpeech()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        if (!_buffer.empty() && _hasNewAudio) {
            _endOfSpeech = true;
            _bufferSizeAtEndOfSpeech = _buffer.size();
        }
    }
    _cv.notify_one();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void WhistleTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener*
WhistleTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void WhistleTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig WhistleTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void WhistleTranscriptionProvider::setInferenceConfig(const InferenceConfig& config)
{
    std::lock_guard<std::mutex> lock(_mtx);
    _inferenceConfig = config;
}

WhistleTranscriptionProvider::InferenceConfig
WhistleTranscriptionProvider::getInferenceConfig() const
{
    std::lock_guard<std::mutex> lock(_mtx);
    return _inferenceConfig;
}

// ---------------------------------------------------------------------------
// Audio format conversion (mirrors the WhisperCpp / Parakeet implementations)
// ---------------------------------------------------------------------------

void WhistleTranscriptionProvider::appendAudioAsFloat(const uint8_t* data, size_t size,
                                                      std::vector<float>& out) const
{
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::FLOAT: {
        size_t numSamples = size / sizeof(float);
        const auto* samples = reinterpret_cast<const float*>(data);
        out.insert(out.end(), samples, samples + numSamples);
        break;
    }
    case va::audio::AudioFormat::INT: {
        size_t numSamples = size / sizeof(int32_t);
        const auto* samples = reinterpret_cast<const int32_t*>(data);
        out.reserve(out.size() + numSamples);
        for (size_t i = 0; i < numSamples; ++i)
            out.push_back(static_cast<float>(samples[i]) / 2147483648.0f);
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        out.reserve(out.size() + size);
        for (size_t i = 0; i < size; ++i)
            out.push_back((static_cast<float>(data[i]) - 128.0f) / 128.0f);
        break;
    }
    default:
        if (_audioConfig.format != va::audio::AudioFormat::SHORT)
            spdlog::warn("[WhistleSTT] Unknown audio format, treating as int16");
        [[fallthrough]];
    case va::audio::AudioFormat::SHORT: {
        size_t numSamples = size / sizeof(int16_t);
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        out.reserve(out.size() + numSamples);
        for (size_t i = 0; i < numSamples; ++i)
            out.push_back(static_cast<float>(samples[i]) / 32768.0f);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

va::audio::TranscriptionResult
WhistleTranscriptionProvider::runInference(const std::vector<float>& audio)
{
    va::audio::TranscriptionResult result;
    if (audio.empty()) return result;

    InferenceConfig config = getInferenceConfig();
    std::string keywords;
    for (const auto& k : config.keywords) {
        if (k.empty()) continue;
        if (!keywords.empty()) keywords += '\n';
        keywords += k;
    }

    std::string json, error;
    if (!NeedleEngine::instance().transcribe(audio, config.language, keywords,
                                             config.wordTimestamps, json, error)) {
        spdlog::error("[WhistleSTT] Transcription failed: {}", error);
        if (_listener) _listener->onTranscriptionError("Whistle transcription failed: " + error);
        return result;
    }

    // {"text":"...","language":"en","ttft_ms":0.0,"decode_tps":0.0,
    //  "words":[{"word":"...","start":0.00,"end":0.00,"probability":0.000}]}
    const auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        spdlog::error("[WhistleSTT] Unexpected engine output: {}", json);
        return result;
    }

    result.text = trim(doc.value("text", std::string{}));
    if (result.text.empty()) return result; // silence and steady noise come back empty

    const std::string language = doc.value("language", std::string{});
    if (!language.empty()) {
        // The engine reports the language it used but no score for it.
        result.languageTags.push_back({ language, 0.0f });
    }

    float probabilitySum = 0.0f;
    auto words = doc.find("words");
    if (words != doc.end() && words->is_array()) {
        for (const auto& w : *words) {
            va::audio::TranscriptionResult::Word word;
            word.text        = trim(w.value("word", std::string{}));
            word.startTimeMs = secondsToMs(w.value("start", 0.0));
            word.endTimeMs   = secondsToMs(w.value("end", 0.0));
            word.confidence  = w.value("probability", 0.0f);
            if (word.text.empty()) continue;
            if (!language.empty()) word.languageTags.push_back({ language, 0.0f });
            probabilitySum += word.confidence;
            result.words.push_back(std::move(word));
        }
    }

    if (!result.words.empty()) {
        result.confidence  = probabilitySum / static_cast<float>(result.words.size());
        result.startTimeMs = result.words.front().startTimeMs;
        result.endTimeMs   = result.words.back().endTimeMs;
    } else {
        // Without word timestamps the engine gives no per-token scores.
        result.confidence = 1.0f;
        const int rate    = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
        result.endTimeMs  = static_cast<int64_t>(audio.size()) * 1000 / rate;
    }

    spdlog::debug("[WhistleSTT] '{}' ({} ms to first token, {} tok/s)", result.text,
                  doc.value("ttft_ms", 0.0), doc.value("decode_tps", 0.0));
    return result;
}

// ---------------------------------------------------------------------------
// Inference loop (background thread)
// ---------------------------------------------------------------------------

void WhistleTranscriptionProvider::inferenceLoop()
{
    auto lastPartialTime = std::chrono::steady_clock::now();

    while (_running) {
        std::vector<float> audio;
        bool doFinal   = false;
        bool doPartial = false;
        size_t minGrowth = 0;

        {
            std::unique_lock<std::mutex> lock(_mtx);
            _cv.wait_for(lock, std::chrono::milliseconds(10));

            if (!_running) break;
            if (_buffer.empty() || !_hasNewAudio) continue;

            auto now       = std::chrono::steady_clock::now();
            auto idleMs    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - _lastWriteTime).count();
            auto partialMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastPartialTime).count();

            if (_endOfSpeech || idleMs >= _inferenceConfig.idleTimeoutMs) {
                audio = std::move(_buffer);
                _buffer.clear();
                // Audio written after commitEndOfSpeech belongs to the next utterance's
                // padding; transcribe the utterance as it stood at the boundary.
                if (_endOfSpeech && _bufferSizeAtEndOfSpeech > 0 && _bufferSizeAtEndOfSpeech < audio.size())
                    audio.resize(_bufferSizeAtEndOfSpeech);
                _hasNewAudio  = false;
                _endOfSpeech  = false;
                _bufferSizeAtEndOfSpeech = 0;
                doFinal       = true;
                minGrowth     = _inferenceConfig.minNewSamplesForFinal;
            } else if (partialMs >= _inferenceConfig.partialIntervalMs) {
                audio     = _buffer; // copy, do not drain
                doPartial = true;
                minGrowth = _inferenceConfig.minNewSamplesForPartial;
            }
        }

        if (!(doFinal || doPartial) || audio.empty()) continue;

        const size_t growth = audio.size() > _lastInferSamples ? audio.size() - _lastInferSamples : 0;
        const bool haveCache = _lastInferSamples > 0;

        va::audio::TranscriptionResult result;
        if (haveCache && growth < minGrowth) {
            if (doPartial) {
                // Nothing new worth re-transcribing and the last partial is already out.
                lastPartialTime = std::chrono::steady_clock::now();
                continue;
            }
            result = _cachedResult;
            spdlog::debug("[WhistleSTT] Reusing cached result for final (growth={} samples)", growth);
        } else {
            result            = runInference(audio);
            _cachedResult     = result;
            _lastInferSamples = audio.size();
        }

        if (doFinal) {
            _cachedResult = {};
            _lastInferSamples = 0;
            if (!result.text.empty() && _listener) _listener->onFinalTranscription(result);
        } else {
            lastPartialTime = std::chrono::steady_clock::now();
            if (!result.text.empty() && _listener) _listener->onPartialTranscription(result);
        }
    }
}
