#include "MoonshineSttProvider.hpp"
#include "impl/MoonshineSttProviderImpl.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// MoonshineSttProvider
// ---------------------------------------------------------------------------

MoonshineSttProvider::MoonshineSttProvider(const std::string& modelDir)
    : _modelDir(modelDir)
{
}

MoonshineSttProvider::~MoonshineSttProvider()
{
    try { if (_running) stopTranscriptionProvider(); } catch (...) {}
    releaseTranscriptionProvider();
}

bool MoonshineSttProvider::initializeTranscriptionProvider()
{
    if (_impl) {
        spdlog::warn("[MoonshineStt] Already initialized");
        return true;
    }

    try {
        _impl = std::make_unique<MoonshineSttProviderImpl>();
        if (!_impl->initialize(_modelDir)) {
            _impl.reset();
            return false;
        }
    }
    catch (const std::exception& e) {
        spdlog::error("[MoonshineStt] Error during init: {}", e.what());
        _impl.reset();
        return false;
    }

    spdlog::info("[MoonshineStt] Initialized from {}", _modelDir);
    return true;
}

void MoonshineSttProvider::releaseTranscriptionProvider()
{
    _impl.reset();
}

bool MoonshineSttProvider::startTranscriptionProvider()
{
    if (_running) return true;
    if (!_impl) {
        spdlog::error("[MoonshineStt] Not initialized");
        return false;
    }

    if (_audioConfig.sampleRate <= 0) {
        spdlog::warn("[MoonshineStt] sampleRate not set - defaulting to 16000 Hz");
        _audioConfig.sampleRate = 16000;
    }

    const size_t maxSamples = static_cast<size_t>(
        _inferenceConfig.maxBufferSeconds * static_cast<float>(_audioConfig.sampleRate));
    _buffer.assign(maxSamples, 0.0f);
    _inferenceBuffer.assign(maxSamples, 0.0f);
    _bufferSize          = 0;
    _inferenceBufferSize = 0;

    _running       = true;
    _hasNewAudio   = false;
    _lastWriteTime = std::chrono::steady_clock::now();
    _thread        = std::thread(&MoonshineSttProvider::inferenceLoop, this);

    spdlog::info("[MoonshineStt] Started");
    return true;
}

void MoonshineSttProvider::stopTranscriptionProvider()
{
    if (!_running) return;
    _running = false;
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();

    std::lock_guard<std::mutex> lock(_mtx);
    std::fill(_buffer.begin(),          _buffer.begin()          + _bufferSize,          0.0f);
    std::fill(_inferenceBuffer.begin(), _inferenceBuffer.begin() + _inferenceBufferSize, 0.0f);
    _bufferSize          = 0;
    _inferenceBufferSize = 0;
    _hasNewAudio         = false;
    _endOfSpeech         = false;

    spdlog::info("[MoonshineStt] Stopped");
}

bool MoonshineSttProvider::isTranscriptionProviderRunning() const
{
    return _running;
}

size_t MoonshineSttProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_running || !data || size == 0) return 0;
    {
        std::lock_guard<std::mutex> lock(_mtx);
        appendAudioAsFloat(data, size);
        _lastWriteTime = std::chrono::steady_clock::now();
        _hasNewAudio   = true;
    }
    _cv.notify_one();
    return size;
}

void MoonshineSttProvider::commitEndOfSpeech()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _endOfSpeech             = true;
        _bufferSizeAtEndOfSpeech = _bufferSize;
    }
    _cv.notify_one();
}

va::audio::TranscriptionResult MoonshineSttProvider::writeFullAudioData(
    const uint8_t* data, size_t size)
{
    std::vector<float> audio(size);
    size_t n = moonshine_impl::convertToFloatInto(
        data, size, _audioConfig.format, audio.data(), audio.size());
    audio.resize(n);
    auto result = runInference(audio.data(), audio.size());

    va::audio::TranscriptionResult tr;
    tr.text       = result.text;
    tr.confidence = result.confidence;
    return tr;
}

void MoonshineSttProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener* MoonshineSttProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void MoonshineSttProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig MoonshineSttProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void MoonshineSttProvider::setInferenceConfig(const InferenceConfig& config)
{
    std::lock_guard<std::mutex> lock(_mtx);
    _inferenceConfig = config;
}

MoonshineSttProvider::InferenceConfig MoonshineSttProvider::getInferenceConfig() const
{
    return _inferenceConfig;
}

void MoonshineSttProvider::clearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(_mtx);
    std::fill(_buffer.begin(), _buffer.begin() + _bufferSize, 0.0f);
    _bufferSize              = 0;
    _hasNewAudio             = false;
    _endOfSpeech             = false;
    _bufferSizeAtEndOfSpeech = 0;
}

void MoonshineSttProvider::setTimingObserver(va::metrics::TimingObserverInterface* observer)
{
    _timingObserver = observer;
}

// ---------------------------------------------------------------------------
// appendAudioAsFloat - must be called under _mtx
// ---------------------------------------------------------------------------

void MoonshineSttProvider::appendAudioAsFloat(const uint8_t* data, size_t size)
{
    const size_t available = _buffer.size() - _bufferSize;
    if (available == 0) {
        spdlog::warn("[MoonshineStt] Audio buffer full ({} samples), dropping audio",
                     _buffer.size());
        return;
    }
    size_t written = moonshine_impl::convertToFloatInto(
        data, size, _audioConfig.format,
        _buffer.data() + _bufferSize, available);
    _bufferSize += written;
}

// ---------------------------------------------------------------------------
// runInference - encode audio then autoregressively decode with KV-cache
// ---------------------------------------------------------------------------

MoonshineSttProvider::InferResult
MoonshineSttProvider::runInference(const float* audioData, size_t numSamples)
{
    auto ir = _impl->runInference(audioData, numSamples,
                                  _audioConfig.sampleRate,
                                  _inferenceConfig.maxTokensPerSecond,
                                  _timingObserver);
    return { ir.text, ir.confidence };
}

// ---------------------------------------------------------------------------
// inferenceLoop
// ---------------------------------------------------------------------------

void MoonshineSttProvider::inferenceLoop()
{
    auto lastPartialTime = std::chrono::steady_clock::now();

    while (_running) {
        bool doFinal   = false;
        bool doPartial = false;
        InferenceConfig cfg;

        {
            std::unique_lock<std::mutex> lock(_mtx);
            cfg = _inferenceConfig;
            _cv.wait_for(lock, std::chrono::milliseconds(cfg.idleTimeoutMs), [this] {
                return !_running || _hasNewAudio || _endOfSpeech;
            });

            if (!_running) break;
            if ((_bufferSize == 0 || !_hasNewAudio) && !_endOfSpeech) continue;

            auto now            = std::chrono::steady_clock::now();
            auto idleMs         = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      now - _lastWriteTime).count();
            auto sincePartialMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      now - lastPartialTime).count();

            if (_endOfSpeech || idleMs >= cfg.idleTimeoutMs) {
                // Swap O(1): inference runs on _inferenceBuffer, new audio accumulates in _buffer
                std::swap(_buffer, _inferenceBuffer);
                _inferenceBufferSize = _bufferSize;
                std::fill(_buffer.begin(), _buffer.begin() + _inferenceBufferSize, 0.0f);
                _bufferSize  = 0;
                _hasNewAudio = false;
                _endOfSpeech = false;
                doFinal      = true;
            }
            else if (sincePartialMs >= cfg.partialIntervalMs) {
                _inferenceBufferSize = _bufferSize;
                doPartial            = true;
            }
        }

        // Copy partial audio outside the lock
        if (doPartial && !doFinal) {
            std::copy(_buffer.begin(), _buffer.begin() + _inferenceBufferSize,
                      _inferenceBuffer.begin());
        }

        if (!(doFinal || doPartial) || !_impl)
            continue;

        // Empty buffer: emit cached partial as final if available
        if (_inferenceBufferSize == 0) {
            if (doFinal && !_cachedPartialResult.empty() && _listener) {
                spdlog::info("[MoonshineStt] EOS with empty buffer - emitting cached partial as final");
                va::audio::TranscriptionResult res;
                res.text       = _cachedPartialResult;
                res.confidence = _cachedPartialConfidence;
                _listener->onFinalTranscription(res);
                _cachedPartialResult.clear();
                _cachedPartialConfidence = 0.0f;
                _lastInferSamples        = 0;
                _bufferSizeAtEndOfSpeech = 0;
            }
            continue;
        }

        const size_t effectiveSize = (doFinal && _bufferSizeAtEndOfSpeech > 0)
            ? _bufferSizeAtEndOfSpeech
            : _inferenceBufferSize;

        const size_t growth = (_lastInferSamples > 0 && effectiveSize > _lastInferSamples)
            ? effectiveSize - _lastInferSamples
            : effectiveSize;

        const bool canReuse = !_cachedPartialResult.empty() &&
            (doFinal ? growth < cfg.minNewSamplesForFinal
                     : growth < cfg.minNewSamplesForPartial);

        std::string text;
        float       confidence;

        if (canReuse) {
            text       = _cachedPartialResult;
            confidence = _cachedPartialConfidence;
            spdlog::debug("[MoonshineStt] Reusing cached result (growth={} samples)", growth);
        }
        else {
            auto ir = runInference(_inferenceBuffer.data(), effectiveSize);
            text       = ir.text;
            confidence = ir.confidence;

            _cachedPartialResult     = text;
            _cachedPartialConfidence = confidence;
            _lastInferSamples        = effectiveSize;
        }

        if (_listener) {
            va::audio::TranscriptionResult res;
            res.text       = text;
            res.confidence = confidence;

            if (doFinal) {
                _listener->onFinalTranscription(res);
                _cachedPartialResult.clear();
                _cachedPartialConfidence = 0.0f;
                _lastInferSamples        = 0;
                _bufferSizeAtEndOfSpeech = 0;
                lastPartialTime          = std::chrono::steady_clock::now();
            }
            else {
                _listener->onPartialTranscription(res);
                lastPartialTime = std::chrono::steady_clock::now();
            }
        }
    }
}
