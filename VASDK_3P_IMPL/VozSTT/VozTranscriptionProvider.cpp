#include "VozTranscriptionProvider.hpp"
#include <spdlog/spdlog.h>
#include <exception>
#include <utility>

namespace va::thirdparty {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VozTranscriptionProvider::VozTranscriptionProvider(VozTranscriptionOptions options)
    : _options(std::move(options))
{
}

VozTranscriptionProvider::~VozTranscriptionProvider()
{
    if (_running) stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool VozTranscriptionProvider::initializeTranscriptionProvider()
{
    if (!_options.recognizer) {
        spdlog::error("[VozSTT] No recognizer supplied");
        return false;
    }
    // The SDK loads the model lazily on its first transcription.
    _initialized = true;
    spdlog::info("[VozSTT] Initialized");
    return true;
}

void VozTranscriptionProvider::releaseTranscriptionProvider()
{
    // The recognizer's owner (the platform SDK wrapper) releases the model.
    _initialized = false;
}

bool VozTranscriptionProvider::startTranscriptionProvider()
{
    if (_running) return true;
    if (!_initialized) {
        spdlog::error("[VozSTT] Not initialized, cannot start");
        return false;
    }
    if (_audioConfig.channels != 1) {
        spdlog::warn("[VozSTT] Audio has {} channels; Voz expects mono", _audioConfig.channels);
    }

    _running = true;
    _hasNewAudio = false;
    _lastWriteTime = std::chrono::steady_clock::now();
    _thread = std::thread(&VozTranscriptionProvider::inferenceLoop, this);

    spdlog::info("[VozSTT] Started");
    return true;
}

void VozTranscriptionProvider::stopTranscriptionProvider()
{
    if (!_running) return;

    _running = false;
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();

    clearAudioBuffer();
    spdlog::info("[VozSTT] Stopped");
}

bool VozTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _running;
}

// ---------------------------------------------------------------------------
// Audio input
// ---------------------------------------------------------------------------

size_t VozTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
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

va::audio::TranscriptionResult VozTranscriptionProvider::writeFullAudioData(const uint8_t* data, size_t size)
{
    if (!_initialized) {
        spdlog::error("[VozSTT] writeFullAudioData called before initialization");
        return {};
    }
    if (!data || size == 0) return {};

    std::vector<float> audio;
    appendAudioAsFloat(data, size, audio);
    return runInference(audio);
}

void VozTranscriptionProvider::commitEndOfSpeech()
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

void VozTranscriptionProvider::clearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(_mtx);
    _buffer.clear();
    _hasNewAudio = false;
    _endOfSpeech = false;
    _bufferSizeAtEndOfSpeech = 0;
    _discardCachedResult = true;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void VozTranscriptionProvider::setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* VozTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void VozTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig VozTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

// ---------------------------------------------------------------------------
// Audio format conversion (mirrors the Whistle / Parakeet implementations)
// ---------------------------------------------------------------------------

void VozTranscriptionProvider::appendAudioAsFloat(const uint8_t* data, size_t size, std::vector<float>& out) const
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
            spdlog::warn("[VozSTT] Unknown audio format, treating as int16");
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

va::audio::TranscriptionResult VozTranscriptionProvider::runInference(const std::vector<float>& audio)
{
    if (audio.empty()) return {};

    const int sampleRate = _audioConfig.sampleRate > 0 ? _audioConfig.sampleRate : 16000;
    va::audio::TranscriptionResult result;
    try {
        result = _options.recognizer(audio, sampleRate);
    } catch (const std::exception& error) {
        spdlog::error("[VozSTT] Transcription failed: {}", error.what());
        if (_listener) _listener->onTranscriptionError(std::string("Voz transcription failed: ") + error.what());
        return {};
    }

    // Voz reports no scores; a transcript it returns is as certain as it gets.
    result.confidence = 1.0f;
    for (auto& word : result.words) word.confidence = 1.0f;
    if (!result.words.empty()) {
        result.startTimeMs = result.words.front().startTimeMs;
        result.endTimeMs   = result.words.back().endTimeMs;
    } else {
        result.endTimeMs = static_cast<int64_t>(audio.size()) * 1000 / sampleRate;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Inference loop (background thread)
// ---------------------------------------------------------------------------

void VozTranscriptionProvider::inferenceLoop()
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
            if (_discardCachedResult) {
                _cachedResult = {};
                _lastInferSamples = 0;
                _discardCachedResult = false;
            }
            if (_buffer.empty() || !_hasNewAudio) continue;

            auto now       = std::chrono::steady_clock::now();
            auto idleMs    = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastWriteTime).count();
            auto partialMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPartialTime).count();

            if (_endOfSpeech || idleMs >= _options.idleTimeoutMs) {
                audio = std::move(_buffer);
                _buffer.clear();
                // Audio written after commitEndOfSpeech belongs to the next utterance.
                if (_endOfSpeech && _bufferSizeAtEndOfSpeech > 0 && _bufferSizeAtEndOfSpeech < audio.size())
                    audio.resize(_bufferSizeAtEndOfSpeech);
                _hasNewAudio  = false;
                _endOfSpeech  = false;
                _bufferSizeAtEndOfSpeech = 0;
                doFinal       = true;
                minGrowth     = _options.minNewSamplesForFinal;
            } else if (_options.partialIntervalMs > 0 && partialMs >= _options.partialIntervalMs) {
                audio     = _buffer; // copy, do not drain
                doPartial = true;
                minGrowth = _options.minNewSamplesForPartial;
            }
        }

        if (!(doFinal || doPartial) || audio.empty()) continue;

        const size_t growth = audio.size() > _lastInferSamples ? audio.size() - _lastInferSamples : 0;
        const bool haveCache = _lastInferSamples > 0;

        va::audio::TranscriptionResult result;
        if (haveCache && growth < minGrowth) {
            if (doPartial) {
                lastPartialTime = std::chrono::steady_clock::now();
                continue;
            }
            result = _cachedResult;
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

} // namespace va::thirdparty
