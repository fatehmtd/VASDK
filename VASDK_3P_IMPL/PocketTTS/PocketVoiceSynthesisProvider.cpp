#include "PocketVoiceSynthesisProvider.hpp"
#include "impl/PocketTTSImpl.hpp"

#include <VASDK/AudioSIMD.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PocketVoiceSynthesisProvider::PocketVoiceSynthesisProvider(
    const std::string& modelDir, const std::string& voiceName)
    : _impl(std::make_shared<PocketTTSImpl>(modelDir, voiceName))
{
}

PocketVoiceSynthesisProvider::~PocketVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

// ---------------------------------------------------------------------------
// GenericVoiceSynthesisProvider interface
// ---------------------------------------------------------------------------

bool PocketVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_running) {
        spdlog::warn("[PocketTTS] Provider already running.");
        return true;
    }
    if (!_impl->initialize()) {
        spdlog::error("[PocketTTS] Initialization failed.");
        return false;
    }
    _running = true;
    _stopWorker.store(false);
    _cancelSynth.store(false);
    _deliveryDone = false;
    _inSynthesisSequence = false;

    _synthThread = std::thread(&PocketVoiceSynthesisProvider::workerLoop, this);
    _deliveryThread = std::thread(&PocketVoiceSynthesisProvider::deliveryLoop, this);

    spdlog::info("[PocketTTS] Provider started (model rate: 24 kHz → pipeline: {} Hz {}ch)",
        _audioConfig.sampleRate, _audioConfig.channels);
    return true;
}

void PocketVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
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
    spdlog::info("[PocketTTS] Provider stopped.");
}

bool PocketVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _running;
}

bool PocketVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (!_running) {
        spdlog::error("[PocketTTS] synthesizeSpeech() called but provider is not running.");
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

void PocketVoiceSynthesisProvider::cancelSynthesis() {
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
    }
    _cancelSynth.store(true);
    _queueCv.notify_all();
    flushDeliverySlot();
}

void PocketVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config) {
    _audioConfig = config;
}

va::audio::AudioConfig PocketVoiceSynthesisProvider::getAudioConfiguration() const {
    return _audioConfig;
}

void PocketVoiceSynthesisProvider::setVoiceSynthesisResultListener(
    va::audio::VoiceSynthesisResultListener* listener) {
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener*
PocketVoiceSynthesisProvider::getVoiceSynthesisResultListener() const {
    return _resultListener;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

void PocketVoiceSynthesisProvider::setLsdSteps(int steps) {
    auto p = _impl->getParams();
    p.lsdSteps = std::max(1, std::min(steps, 10));
    _impl->setParams(p);
}

int PocketVoiceSynthesisProvider::getLsdSteps() const {
    return _impl->getParams().lsdSteps;
}

void PocketVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    auto p = _impl->getParams();
    p.silenceDuration = std::max(0.0f, seconds);
    _impl->setParams(p);
}

float PocketVoiceSynthesisProvider::getSilenceDuration() const {
    return _impl->getParams().silenceDuration;
}

void PocketVoiceSynthesisProvider::setDecoderBatchFrames(int frames) {
    auto p = _impl->getParams();
    p.decoderBatchFrames = std::max(1, frames);
    _impl->setParams(p);
}

int PocketVoiceSynthesisProvider::getDecoderBatchFrames() const {
    return _impl->getParams().decoderBatchFrames;
}

bool PocketVoiceSynthesisProvider::setVoice(const std::string& voiceName) {
    auto names = _impl->getVoiceNames();
    bool found = std::any_of(names.begin(), names.end(),
        [&](const std::string& n) { return n == voiceName; });
    auto p = _impl->getParams();
    p.voiceName = voiceName;
    _impl->setParams(p);
    if (!found) spdlog::warn("[PocketTTS] Voice '{}' not in voices.bin; will fall back.", voiceName);
    return found;
}

std::string PocketVoiceSynthesisProvider::getVoice() const {
    return _impl->getParams().voiceName;
}

std::vector<std::string> PocketVoiceSynthesisProvider::getAvailableVoices() const {
    return _impl->getVoiceNames();
}

// ---------------------------------------------------------------------------
// Worker thread (synthesis queue → synthesizeOne)
// Mirrors SupertonicVoiceSynthesisProvider::workerLoop exactly.
// ---------------------------------------------------------------------------

void PocketVoiceSynthesisProvider::workerLoop() {
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

        VASDK_CTRACK_NAME("PocketTTS::processChunk");
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
// synthesizeOne - runs ONNX inference, hands audio chunks to delivery thread
// ---------------------------------------------------------------------------

bool PocketVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    VASDK_CTRACK_NAME("PocketTTS::synthesize");
    try {

        if (_preBufferedSamplesMS > 0) {
            // On Android the CPU is thermally throttled between utterances (DVFS idle).  The first
            // ~16 ORT inference calls (3 AR steps × 4 ODE steps + 1 mimi decode) that produce
            // kFirstChunkFrames=3 latent frames only yield ~40 ms of audio.  On a cold CPU those
            // calls can take >40 ms, draining the playback queue and causing audible choppiness at
            // the start of every synthesis.
            //
            // Fix: accumulate kPreBufferSamples of decoded audio before handing anything to the
            // delivery thread.  Playback starts with ~200 ms already buffered, providing enough
            // headroom for the CPU to ramp up frequency and reach real-time decoding speed.
            // kPreBufferSamples is expressed in the model's native 24 kHz sample rate.
            const int kPreBufferSamples = kModelSampleRate * _preBufferedSamplesMS / 1000; // 500 ms @ 24 kHz

            std::vector<float> preBuffer;
            bool               preBufferFlushed = false;

            _impl->synthesize(text, _cancelSynth,
                [this, &preBuffer, &preBufferFlushed, kPreBufferSamples](std::vector<float> chunk) {
                    if (_cancelSynth.load()) return;
                    if (!preBufferFlushed) {
                        preBuffer.insert(preBuffer.end(), chunk.begin(), chunk.end());
                        if (static_cast<int>(preBuffer.size()) >= kPreBufferSamples) {
                            preBufferFlushed = true;
                            handoffAudio(std::move(preBuffer));
                        }
                        return;
                    }
                    handoffAudio(std::move(chunk));
                });

            // Flush any audio that was accumulated but never reached the threshold
            // (short utterances that finish before filling the pre-buffer).
            if (!preBuffer.empty() && !_cancelSynth.load()) {
                handoffAudio(std::move(preBuffer));
            }
        }
        else {
            _impl->synthesize(text, _cancelSynth,
                [this](std::vector<float> chunk) {
                    if (!_cancelSynth.load())
                        handoffAudio(std::move(chunk));
                });
        }

        // Wait for delivery thread to consume the last slot before signalling completion.
        std::unique_lock<std::mutex> lock(_deliveryMutex);
        _deliveryCv.wait(lock, [this] {
            return !_deliverySlot.has_value() || _cancelSynth.load();
            });
        return !_cancelSynth.load();
    }
    catch (const std::exception& e) {
        spdlog::error("[PocketTTS] Synthesis failed: {}", e.what());
        if (_resultListener) _resultListener->onSynthesisError(e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Delivery pipeline (double-buffered synthesis ↔ audio delivery overlap)
// Mirrors SupertonicVoiceSynthesisProvider's delivery pipeline exactly.
// ---------------------------------------------------------------------------

void PocketVoiceSynthesisProvider::handoffAudio(std::vector<float> wav) {
    std::unique_lock<std::mutex> lock(_deliveryMutex);
    _deliveryCv.wait(lock, [this] {
        return !_deliverySlot.has_value() || _cancelSynth.load();
        });
    if (_cancelSynth.load()) return;
    _deliverySlot = std::move(wav);
    _deliveryReadyCv.notify_one();
}

void PocketVoiceSynthesisProvider::flushDeliverySlot() {
    std::lock_guard<std::mutex> lock(_deliveryMutex);
    _deliverySlot.reset();
    _deliveryCv.notify_one();
}

void PocketVoiceSynthesisProvider::deliveryLoop() {
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
// Resampling (Lanczos windowed-sinc, same as SupertonicVoiceSynthesisProvider)
// ---------------------------------------------------------------------------

static constexpr double kPi = 3.14159265358979323846;

static inline double sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    double px = kPi * x;
    return std::sin(px) / px;
}

static void resample(const std::vector<float>& in, int srcRate, int dstRate,
    std::vector<float>& out) {
    if (in.empty() || srcRate <= 0 || dstRate <= 0) { out.clear(); return; }
    const int    A = 3;
    const double ratio = static_cast<double>(dstRate) / srcRate;
    const double filterScale = std::min(1.0, ratio);
    const int    kernelR = static_cast<int>(std::ceil(A / filterScale));
    const size_t outLen = static_cast<size_t>(std::ceil(in.size() * ratio));
    out.resize(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        double center = i / ratio;
        int left = static_cast<int>(std::floor(center)) - kernelR + 1;
        int right = static_cast<int>(std::floor(center)) + kernelR;
        double sample = 0.0, wSum = 0.0;
        for (int j = left; j <= right; ++j) {
            double x = (center - j) * filterScale;
            double w = (std::abs(x) < A) ? sinc(x) * sinc(x / A) : 0.0;
            if (j >= 0 && j < static_cast<int>(in.size())) {
                sample += in[j] * w; wSum += w;
            }
        }
        out[i] = (wSum > 0.0) ? static_cast<float>(sample / wSum) : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// deliverAudio - resample + format convert + call listener
// Mirrors SupertonicVoiceSynthesisProvider::deliverAudio exactly.
// ---------------------------------------------------------------------------

void PocketVoiceSynthesisProvider::deliverAudio(const std::vector<float>& pcmIn) {
    if (!_resultListener || pcmIn.empty()) return;

    const std::vector<float>* pcm = &pcmIn;
    if (kModelSampleRate != _audioConfig.sampleRate &&
        kModelSampleRate > 0 && _audioConfig.sampleRate > 0) {
        resample(pcmIn, kModelSampleRate, _audioConfig.sampleRate, _resampleBuf);
        pcm = &_resampleBuf;
    }

    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT: {
        _int16Buf.resize(pcm->size());
        AudioSIMD::convertFloatToInt16(pcm->data(), _int16Buf.data(), pcm->size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int16Buf.data()),
            _int16Buf.size() * sizeof(int16_t));
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm->data()),
            pcm->size() * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        _int32Buf.resize(pcm->size());
        AudioSIMD::convertFloatToInt32(pcm->data(), _int32Buf.data(), pcm->size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int32Buf.data()),
            _int32Buf.size() * sizeof(int32_t));
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        _uint8Buf.resize(pcm->size());
        AudioSIMD::convertFloatToUint8(pcm->data(), _uint8Buf.data(), pcm->size());
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_uint8Buf.data()),
            _uint8Buf.size() * sizeof(uint8_t));
        break;
    }
    default:
        spdlog::warn("[PocketTTS] Unknown audio format; delivering as float.");
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm->data()),
            pcm->size() * sizeof(float));
        break;
    }
}

// ---------------------------------------------------------------------------
// Text normalisation (delegates to PocketTTSImpl static helpers, exposed here
// to allow pre-processing before enqueuing so the worker thread doesn't
// stall on normalization).
// ---------------------------------------------------------------------------

/*static*/ std::string PocketVoiceSynthesisProvider::normalizeText(const std::string& text) {
    return PocketTTSImpl::normalizeText(text);
}
