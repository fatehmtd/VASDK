#include "MoonshineSttProvider.hpp"
#include "impl/MoonshineSttProviderImpl.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <utility>

namespace va::thirdparty::stt {

// ---------------------------------------------------------------------------
// MoonshineSttProvider
// ---------------------------------------------------------------------------

MoonshineSttProvider::MoonshineSttProvider(MoonshineSttOptions options) : _options(std::move(options)) {}

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
        if (!_impl->initialize(_options.modelDir)) {
            _impl.reset();
            return false;
        }
        _impl->setTimingObserver(_timingObserver);
    }
    catch (const std::exception& e) {
        spdlog::error("[MoonshineStt] Error during init: {}", e.what());
        _impl.reset();
        return false;
    }

    spdlog::info("[MoonshineStt] Initialized from {}", _options.modelDir);
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
    if (_audioConfig.sampleRate != 16000) {
        spdlog::error("[MoonshineStt] Moonshine needs 16 kHz input, got {} Hz", _audioConfig.sampleRate);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mtx);
        _incoming.clear();
        _endOfSpeech    = false;
        _resetRequested = false;
        _lastWriteTime  = std::chrono::steady_clock::now();
    }
    _running = true;
    _thread  = std::thread(&MoonshineSttProvider::inferenceLoop, this);

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
    _incoming.clear();
    _endOfSpeech    = false;
    _resetRequested = false;

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
        const size_t old = _incoming.size();
        _incoming.resize(old + size);  // upper bound: BYTE format is 1 sample per byte
        _incoming.resize(old + moonshine_impl::convertToFloatInto(
                                   data, size, _audioConfig.format, _incoming.data() + old, size));
        _lastWriteTime = std::chrono::steady_clock::now();
    }
    _cv.notify_one();
    return size;
}

void MoonshineSttProvider::commitEndOfSpeech()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _endOfSpeech = true;
    }
    _cv.notify_one();
}

va::audio::TranscriptionResult MoonshineSttProvider::writeFullAudioData(
    const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult tr;
    if (!_impl) {
        spdlog::error("[MoonshineStt] Not initialized");
        return tr;
    }
    std::vector<float> audio(size);
    audio.resize(moonshine_impl::convertToFloatInto(
        data, size, _audioConfig.format, audio.data(), audio.size()));

    // Own stream state, so this is safe alongside the streaming thread.
    MoonshineSttProviderImpl::Stream stream(*_impl);
    if (stream.addAudio(audio.data(), audio.size()))
        tr.text = stream.finish();
    tr.confidence = tr.text.empty() ? 0.0f : 1.0f;
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

void MoonshineSttProvider::clearAudioBuffer()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        _incoming.clear();
        _endOfSpeech    = false;
        _resetRequested = true;  // stream state belongs to the inference thread
    }
    _cv.notify_one();
}

void MoonshineSttProvider::setTimingObserver(va::metrics::TimingObserverInterface* observer)
{
    _timingObserver = observer;
    if (_impl) _impl->setTimingObserver(observer);
}

// ---------------------------------------------------------------------------
// emit - deliver a result to the listener
// ---------------------------------------------------------------------------

void MoonshineSttProvider::emit(const std::string& text, bool isFinal)
{
    if (!_listener) return;
    va::audio::TranscriptionResult res;
    res.text = text;
    // shortcut: decode_full exposes no logits, so confidence is 1/0 by presence of text; derive it from logits if a consumer starts thresholding on it.
    res.confidence = text.empty() ? 0.0f : 1.0f;
    if (isFinal) _listener->onFinalTranscription(res);
    else         _listener->onPartialTranscription(res);
}

// ---------------------------------------------------------------------------
// inferenceLoop - encodes audio as it arrives, decodes partials, finalizes on EOS/idle
// ---------------------------------------------------------------------------

void MoonshineSttProvider::inferenceLoop()
{
    MoonshineSttProviderImpl::Stream stream(*_impl);
    std::vector<float> chunk;
    auto lastPartialTime = std::chrono::steady_clock::now();
    bool newSinceDecode  = false;

    while (_running) {
        bool finalize = false, reset = false, idle = false;
        const MoonshineSttOptions& cfg = _options;
        {
            std::unique_lock<std::mutex> lock(_mtx);
            _cv.wait_for(lock, std::chrono::milliseconds(std::min(cfg.idleTimeoutMs, cfg.partialIntervalMs)),
                         [this] { return !_running || !_incoming.empty() || _endOfSpeech || _resetRequested; });
            if (!_running) break;

            chunk.swap(_incoming);
            reset    = std::exchange(_resetRequested, false);
            finalize = std::exchange(_endOfSpeech, false);
            idle     = std::chrono::steady_clock::now() - _lastWriteTime >=
                       std::chrono::milliseconds(cfg.idleTimeoutMs);
        }

        if (reset) {
            stream.reset();
            newSinceDecode = false;
        }
        if (!chunk.empty()) {
            if (!stream.addAudio(chunk.data(), chunk.size())) stream.reset();
            else newSinceDecode = true;
            chunk.clear();
        }

        if (!stream.hasAudio()) {
            if (finalize) stream.reset();  // drop a sub-chunk tail with nothing encoded
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (finalize || idle) {
            emit(stream.finish(), true);
            newSinceDecode  = false;
            lastPartialTime = now;
        }
        else if (newSinceDecode &&
                 now - lastPartialTime >= std::chrono::milliseconds(cfg.partialIntervalMs)) {
            emit(stream.decode(), false);
            newSinceDecode  = false;
            lastPartialTime = now;
        }
    }
}

} // namespace va::thirdparty::stt
