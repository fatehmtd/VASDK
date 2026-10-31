#include "MossVoiceSynthesisProvider.hpp"
#include "impl/MossTTSImpl.hpp"

#include <VASDK/AudioSIMD.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <cmath>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MossVoiceSynthesisProvider::MossVoiceSynthesisProvider(
    const std::string& modelDir, const std::string& voiceName, int numThreads)
    : _impl(std::make_shared<MossTTSImpl>(modelDir, voiceName, numThreads))
{
}

MossVoiceSynthesisProvider::~MossVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

// ---------------------------------------------------------------------------
// GenericVoiceSynthesisProvider interface
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_running) {
        spdlog::warn("[MossTTS] Provider already running.");
        return true;
    }
    if (!_impl->initialize()) {
        spdlog::error("[MossTTS] Initialization failed.");
        return false;
    }
    _modelSampleRate = _impl->getSampleRate();
    _running = true;
    _stopWorker.store(false);
    _cancelSynth.store(false);
    _deliveryDone = false;
    _inSynthesisSequence = false;

    _synthThread    = std::thread(&MossVoiceSynthesisProvider::workerLoop,   this);
    _deliveryThread = std::thread(&MossVoiceSynthesisProvider::deliveryLoop, this);

    spdlog::info("[MossTTS] Provider started (model rate: {} Hz → pipeline: {} Hz {}ch, voice: {})",
        _modelSampleRate, _audioConfig.sampleRate, _audioConfig.channels, getVoice());
    return true;
}

void MossVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
    if (!_running) return;

    _cancelSynth.store(true);
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
        _stopWorker.store(true);
    }
    _queueCv.notify_all();
    if (_synthThread.joinable()) _synthThread.join();

    flushDeliverySlot();
    {
        std::lock_guard<std::mutex> lock(_deliveryMutex);
        _deliveryDone = true;
    }
    _deliveryReadyCv.notify_all();
    if (_deliveryThread.joinable()) _deliveryThread.join();

    _running = false;
    spdlog::info("[MossTTS] Provider stopped.");
}

bool MossVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _running;
}

bool MossVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (!_running) {
        spdlog::error("[MossTTS] synthesizeSpeech() called but provider is not running.");
        return false;
    }
    if (text.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _textQueue.push(text);
    }
    _queueCv.notify_one();
    return true;
}

void MossVoiceSynthesisProvider::cancelSynthesis() {
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
    }
    _cancelSynth.store(true);
    _queueCv.notify_all();
    flushDeliverySlot();
}

void MossVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config) {
    _audioConfig = config;
}

va::audio::AudioConfig MossVoiceSynthesisProvider::getAudioConfiguration() const {
    return _audioConfig;
}

void MossVoiceSynthesisProvider::setVoiceSynthesisResultListener(
    va::audio::VoiceSynthesisResultListener* listener) {
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener*
MossVoiceSynthesisProvider::getVoiceSynthesisResultListener() const {
    return _resultListener;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::setVoice(const std::string& voiceName) {
    auto p = _impl->getParams();
    p.voiceName = voiceName;
    _impl->setParams(p);
    const auto names = _impl->getVoiceNames();
    const bool found = std::find(names.begin(), names.end(), voiceName) != names.end();
    if (!found && !names.empty())
        spdlog::warn("[MossTTS] Voice '{}' not in manifest; will fall back to '{}'.", voiceName, names.front());
    return found;
}

std::string MossVoiceSynthesisProvider::getVoice() const {
    return _impl->getParams().voiceName;
}

std::vector<std::string> MossVoiceSynthesisProvider::getAvailableVoices() const {
    return _impl->getVoiceNames();
}

void MossVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    auto p = _impl->getParams();
    p.silenceDuration = std::max(0.0f, seconds);
    _impl->setParams(p);
}

float MossVoiceSynthesisProvider::getSilenceDuration() const {
    return _impl->getParams().silenceDuration;
}

void MossVoiceSynthesisProvider::setMaxNewFrames(int frames) {
    auto p = _impl->getParams();
    p.maxNewFrames = std::max(0, frames);
    _impl->setParams(p);
}

int MossVoiceSynthesisProvider::getMaxNewFrames() const {
    return _impl->getParams().maxNewFrames;
}

void MossVoiceSynthesisProvider::setMaxChunkTokens(int tokens) {
    auto p = _impl->getParams();
    p.maxChunkTokens = std::max(1, tokens);
    _impl->setParams(p);
}

int MossVoiceSynthesisProvider::getMaxChunkTokens() const {
    return _impl->getParams().maxChunkTokens;
}

void MossVoiceSynthesisProvider::setTextNormalization(bool enabled) {
    auto p = _impl->getParams();
    p.normalizeText = enabled;
    _impl->setParams(p);
}

bool MossVoiceSynthesisProvider::getTextNormalization() const {
    return _impl->getParams().normalizeText;
}

// ---------------------------------------------------------------------------
// Worker thread (synthesis queue → synthesizeOne)
// Mirrors PocketVoiceSynthesisProvider::workerLoop.
// ---------------------------------------------------------------------------

void MossVoiceSynthesisProvider::workerLoop() {
    while (true) {
        std::string text;
        bool isSequenceStart = false;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueCv.wait(lock, [this] {
                return !_textQueue.empty() || _stopWorker.load();
                });
            if (_stopWorker.load() && _textQueue.empty()) break;
            if (_textQueue.empty()) continue;
            text = std::move(_textQueue.front());
            _textQueue.pop();
            isSequenceStart = !_inSynthesisSequence;
            _inSynthesisSequence = true;
        }

        VASDK_CTRACK_NAME("MossTTS::processChunk");
        if (isSequenceStart) {
            _cancelSynth.store(false);
        }
        else if (_cancelSynth.load()) {
            std::lock_guard<std::mutex> lock(_queueMutex);
            while (!_textQueue.empty()) _textQueue.pop();
            _inSynthesisSequence = false;
            continue;
        }

        if (isSequenceStart && _resultListener)
            _resultListener->onSynthesisStarted();

        bool completed = synthesizeOne(text);

        bool shouldFireCompleted;
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            if (!completed || _cancelSynth.load()) {
                while (!_textQueue.empty()) _textQueue.pop();
                _inSynthesisSequence = false;
                shouldFireCompleted = false;
            }
            else if (_textQueue.empty()) {
                _inSynthesisSequence = false;
                shouldFireCompleted = true;
            }
            else {
                shouldFireCompleted = false;
            }
        }

        if (shouldFireCompleted && _resultListener)
            _resultListener->onSynthesisCompleted();
    }
}

// ---------------------------------------------------------------------------
// synthesizeOne - runs ONNX inference, resamples, hands chunks to delivery thread
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    VASDK_CTRACK_NAME("MossTTS::synthesize");
    try {
        _resampler.reset(_modelSampleRate, _audioConfig.sampleRate);
        const bool resample = !_resampler.passthrough();
        const int  outRate  = resample ? _audioConfig.sampleRate : _modelSampleRate;

        const size_t preBufferSamples = static_cast<size_t>(outRate) * _preBufferedSamplesMS / 1000;
        std::vector<float> preBuffer;
        bool preBufferFlushed = preBufferSamples == 0;

        auto forward = [&](std::vector<float> pcm) {
            if (pcm.empty() || _cancelSynth.load()) return;
            if (!preBufferFlushed) {
                preBuffer.insert(preBuffer.end(), pcm.begin(), pcm.end());
                if (preBuffer.size() >= preBufferSamples) {
                    preBufferFlushed = true;
                    handoffAudio(std::move(preBuffer));
                }
                return;
            }
            handoffAudio(std::move(pcm));
        };

        _impl->synthesize(text, _cancelSynth, [&](std::vector<float> chunk) {
            if (_cancelSynth.load()) return;
            if (!resample) { forward(std::move(chunk)); return; }
            std::vector<float> out;
            _resampler.process(chunk.data(), chunk.size(), out);
            forward(std::move(out));
        });

        if (resample && !_cancelSynth.load()) {
            std::vector<float> tail;
            _resampler.flush(tail);
            forward(std::move(tail));
        }
        // Short utterances can finish before filling the pre-buffer.
        if (!preBuffer.empty() && !_cancelSynth.load())
            handoffAudio(std::move(preBuffer));

        // Wait for delivery thread to consume the last slot before signalling completion.
        std::unique_lock<std::mutex> lock(_deliveryMutex);
        _deliveryCv.wait(lock, [this] {
            return !_deliverySlot.has_value() || _cancelSynth.load();
            });
        return !_cancelSynth.load();
    }
    catch (const std::exception& e) {
        spdlog::error("[MossTTS] Synthesis failed: {}", e.what());
        if (_resultListener) _resultListener->onSynthesisError(e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Delivery pipeline (double-buffered synthesis ↔ audio delivery overlap)
// ---------------------------------------------------------------------------

void MossVoiceSynthesisProvider::handoffAudio(std::vector<float> wav) {
    std::unique_lock<std::mutex> lock(_deliveryMutex);
    _deliveryCv.wait(lock, [this] {
        return !_deliverySlot.has_value() || _cancelSynth.load();
        });
    if (_cancelSynth.load()) return;
    _deliverySlot = std::move(wav);
    _deliveryReadyCv.notify_one();
}

void MossVoiceSynthesisProvider::flushDeliverySlot() {
    std::lock_guard<std::mutex> lock(_deliveryMutex);
    _deliverySlot.reset();
    _deliveryCv.notify_one();
}

void MossVoiceSynthesisProvider::deliveryLoop() {
    while (true) {
        std::vector<float> wav;
        {
            std::unique_lock<std::mutex> lock(_deliveryMutex);
            _deliveryReadyCv.wait(lock, [this] {
                return _deliverySlot.has_value() || _deliveryDone;
                });
            if (_deliveryDone && !_deliverySlot.has_value()) break;
            wav = std::move(*_deliverySlot);
            _deliverySlot.reset();
            _deliveryCv.notify_one();
        }
        deliverAudio(wav);
    }
}

// ---------------------------------------------------------------------------
// StreamResampler - Lanczos (a = 3) windowed sinc, as in PocketVoiceSynthesisProvider,
// but carrying input history between calls.
// ---------------------------------------------------------------------------

static inline double sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    const double px = 3.14159265358979323846 * x;
    return std::sin(px) / px;
}

void MossVoiceSynthesisProvider::StreamResampler::reset(int srcRate, int dstRate) {
    _src = srcRate;
    _dst = dstRate;
    _buf.clear();
    _bufStart = 0;
    _nextOut  = 0;
    if (passthrough()) return;
    const double ratio = static_cast<double>(dstRate) / srcRate;
    _step        = static_cast<double>(srcRate) / dstRate;
    _filterScale = std::min(1.0, ratio);
    _kernelR     = static_cast<int>(std::ceil(_lanczosA / _filterScale));
}

void MossVoiceSynthesisProvider::StreamResampler::produce(std::vector<float>& out, int64_t inputEnd) {
    while (true) {
        const double  center = _nextOut * _step;
        const int64_t c0     = static_cast<int64_t>(std::floor(center));
        if (c0 + _kernelR >= inputEnd) break;   // need the full right half of the kernel
        double sample = 0.0, wSum = 0.0;
        for (int64_t j = c0 - _kernelR + 1; j <= c0 + _kernelR; ++j) {
            if (j < 0) continue;                  // before stream start
            const double x = (center - j) * _filterScale;
            const double w = (std::abs(x) < _lanczosA) ? sinc(x) * sinc(x / _lanczosA) : 0.0;
            sample += _buf[static_cast<size_t>(j - _bufStart)] * w;
            wSum   += w;
        }
        out.push_back(wSum > 0.0 ? static_cast<float>(sample / wSum) : 0.0f);
        ++_nextOut;
    }
    // Drop input no longer reachable by the kernel of the next output sample.
    const int64_t keepFrom = static_cast<int64_t>(std::floor(_nextOut * _step)) - _kernelR + 1;
    if (keepFrom > _bufStart) {
        const int64_t drop = std::min<int64_t>(keepFrom - _bufStart, static_cast<int64_t>(_buf.size()));
        _buf.erase(_buf.begin(), _buf.begin() + drop);
        _bufStart += drop;
    }
}

void MossVoiceSynthesisProvider::StreamResampler::process(const float* in, size_t n, std::vector<float>& out) {
    _buf.insert(_buf.end(), in, in + n);
    produce(out, _bufStart + static_cast<int64_t>(_buf.size()));
}

void MossVoiceSynthesisProvider::StreamResampler::flush(std::vector<float>& out) {
    const int64_t totalIn  = _bufStart + static_cast<int64_t>(_buf.size());
    const int64_t totalOut = static_cast<int64_t>(std::ceil(totalIn * (static_cast<double>(_dst) / _src)));
    _buf.insert(_buf.end(), static_cast<size_t>(_kernelR) + 1, 0.0f);
    std::vector<float> tail;
    produce(tail, _bufStart + static_cast<int64_t>(_buf.size()));
    const int64_t keep = std::max<int64_t>(0, std::min<int64_t>(static_cast<int64_t>(tail.size()),
                                                                totalOut - (_nextOut - static_cast<int64_t>(tail.size()))));
    out.insert(out.end(), tail.begin(), tail.begin() + keep);
}

// ---------------------------------------------------------------------------
// deliverAudio - format convert + call listener (audio is already at the pipeline rate)
// ---------------------------------------------------------------------------

void MossVoiceSynthesisProvider::deliverAudio(const std::vector<float>& pcm) {
    if (!_resultListener || pcm.empty()) return;

    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT: {
        _int16Buf.resize(pcm.size());
        AudioSIMD::convertFloatToInt16(pcm.data(), _int16Buf.data(), pcm.size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int16Buf.data()),
            _int16Buf.size() * sizeof(int16_t));
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm.data()),
            pcm.size() * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        _int32Buf.resize(pcm.size());
        AudioSIMD::convertFloatToInt32(pcm.data(), _int32Buf.data(), pcm.size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int32Buf.data()),
            _int32Buf.size() * sizeof(int32_t));
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        _uint8Buf.resize(pcm.size());
        AudioSIMD::convertFloatToUint8(pcm.data(), _uint8Buf.data(), pcm.size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_uint8Buf.data()),
            _uint8Buf.size() * sizeof(uint8_t));
        break;
    }
    default:
        spdlog::warn("[MossTTS] Unknown audio format; delivering as float.");
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm.data()),
            pcm.size() * sizeof(float));
        break;
    }
}
