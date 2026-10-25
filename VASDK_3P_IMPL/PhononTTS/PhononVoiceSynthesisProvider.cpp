#include "PhononVoiceSynthesisProvider.hpp"

#include "impl/phonon.h"
#include "impl/normalize.h"
#include "impl/chunk.h"

#include <VASDK/AudioSIMD.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {

std::string join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (const auto& s : items) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// Lanczos windowed-sinc, same kernel as PocketVoiceSynthesisProvider's resampler.
constexpr int    kLanczosA = 3;
constexpr double kPi       = 3.14159265358979323846;

inline double sinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    double px = kPi * x;
    return std::sin(px) / px;
}

int kernelRadius(int srcRate, int dstRate) {
    const double ratio       = static_cast<double>(dstRate) / srcRate;
    const double filterScale = std::min(1.0, ratio);
    return static_cast<int>(std::ceil(kLanczosA / filterScale));
}

} // namespace

// ---------------------------------------------------------------------------
// Engine listener: the engine calls it on its own stream thread while the worker
// thread waits in waitForStream().
// ---------------------------------------------------------------------------

class PhononVoiceSynthesisProvider::StreamListener : public gradium::tts::SynthesisListener {
public:
    explicit StreamListener(PhononVoiceSynthesisProvider* owner) : _owner(owner) {}

    void onStart(gradium::tts::StreamId, const std::string&) override {}

    void onAudioChunk(gradium::tts::StreamId, std::vector<float> chunk) override {
        if (_owner->_cancelSynth.load()) return;
        _owner->onModelAudio(chunk.data(), chunk.size());
    }

    void onFinish(gradium::tts::StreamId, const std::vector<float>&,
                  const gradium::tts::SynthesisStats& stats) override {
        spdlog::debug("[PhononTTS] {:.2f} s of audio in {:.1f} ms (RTF {:.3f})",
            stats.duration_sec, stats.synthesis_ms, stats.rtf);
    }

    void onError(gradium::tts::StreamId, const std::string& message) override {
        failed = true;
        error  = message;
    }

    bool        failed = false;
    std::string error;

private:
    PhononVoiceSynthesisProvider* _owner;
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PhononVoiceSynthesisProvider::PhononVoiceSynthesisProvider(
    const std::string& modelDir, const std::string& voiceName, const std::string& language)
    : _modelDir(modelDir), _voice(voiceName),
      _ruleBits(gradium::tts::text::Rules::defaults().bits)
{
    if (!setLanguage(language))
        spdlog::warn("[PhononTTS] Unknown language '{}'; normalizing as English.", language);
}

PhononVoiceSynthesisProvider::~PhononVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

// ---------------------------------------------------------------------------
// GenericVoiceSynthesisProvider interface
// ---------------------------------------------------------------------------

bool PhononVoiceSynthesisProvider::startVoiceSynthesisProvider() {
    if (_running) {
        spdlog::warn("[PhononTTS] Provider already running.");
        return true;
    }

    // Loading the model is the slow part; keep the engine across stop/start.
    if (!_engine) {
        scanVoices();
        std::string voice;
        {
            std::lock_guard<std::mutex> lock(_settingsMutex);
            if (_availableVoices.empty()) {
                spdlog::error("[PhononTTS] No voices in '{}' (expected default-voice.safetensors "
                              "or voices/*.safetensors).", _modelDir);
                return false;
            }
            if (std::find(_availableVoices.begin(), _availableVoices.end(), _voice) == _availableVoices.end()) {
                const bool hasDefault = std::find(_availableVoices.begin(), _availableVoices.end(),
                                                  "default") != _availableVoices.end();
                const std::string fallback = hasDefault ? "default" : _availableVoices.front();
                if (!_voice.empty())
                    spdlog::warn("[PhononTTS] Voice '{}' not found; using '{}'. Available: {}",
                        _voice, fallback, join(_availableVoices, ", "));
                _voice = fallback;
            }
            voice = _voice;
        }

        const std::filesystem::path dir(_modelDir);
        gradium::tts::Paths paths;
        paths.model_gguf  = (dir / "model.q8.gguf").string();
        paths.config_json = (dir / "config.json").string();
        paths.tokenizer   = (dir / "tokenizer.model").string();
        paths.voice       = voicePath(voice);
        try {
            _engine = std::make_unique<gradium::tts::PhononTTS>(paths);
        }
        catch (const std::exception& e) {
            spdlog::error("[PhononTTS] Failed to load model from '{}': {}", _modelDir, e.what());
            return false;
        }
        _loadedVoice = voice;
        spdlog::info("[PhononTTS] Model loaded from '{}' (voice '{}', {} voices available)",
            _modelDir, voice, _availableVoices.size());
    }

    _running = true;
    _stopWorker.store(false);
    _cancelSynth.store(false);
    _inSynthesisSequence = false;

    _synthThread = std::thread(&PhononVoiceSynthesisProvider::workerLoop, this);

    spdlog::info("[PhononTTS] Provider started (model rate: 24 kHz → pipeline: {} Hz {}ch)",
        _audioConfig.sampleRate, _audioConfig.channels);
    return true;
}

void PhononVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
    if (!_running) return;

    cancelSynthesis();
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _stopWorker.store(true);
    }
    _queueCv.notify_all();
    if (_synthThread.joinable()) _synthThread.join();

    _running = false;
    spdlog::info("[PhononTTS] Provider stopped.");
}

bool PhononVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
    return _running;
}

bool PhononVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
    if (!_running) {
        spdlog::error("[PhononTTS] synthesizeSpeech() called but provider is not running.");
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

void PhononVoiceSynthesisProvider::cancelSynthesis() {
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_textQueue.empty()) _textQueue.pop();
    }
    _cancelSynth.store(true);
    // Generation stops at the next frame boundary; a stale id is a no-op in the engine.
    const uint64_t id = _activeStream.load();
    if (id != 0 && _engine) _engine->cancelStream(id);
    _queueCv.notify_all();
}

void PhononVoiceSynthesisProvider::setAudioConfiguration(const va::audio::AudioConfig& config) {
    _audioConfig = config;
}

va::audio::AudioConfig PhononVoiceSynthesisProvider::getAudioConfiguration() const {
    return _audioConfig;
}

void PhononVoiceSynthesisProvider::setVoiceSynthesisResultListener(
    va::audio::VoiceSynthesisResultListener* listener) {
    _resultListener = listener;
}

va::audio::VoiceSynthesisResultListener*
PhononVoiceSynthesisProvider::getVoiceSynthesisResultListener() const {
    return _resultListener;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

bool PhononVoiceSynthesisProvider::setVoice(const std::string& voiceName) {
    std::lock_guard<std::mutex> lock(_settingsMutex);
    const bool known = _availableVoices.empty() ||
        std::find(_availableVoices.begin(), _availableVoices.end(), voiceName) != _availableVoices.end();
    if (!known) {
        spdlog::warn("[PhononTTS] Voice '{}' not in model; keeping '{}'.", voiceName, _voice);
        return false;
    }
    _voice = voiceName;
    return true;
}

std::string PhononVoiceSynthesisProvider::getVoice() const {
    std::lock_guard<std::mutex> lock(_settingsMutex);
    return _voice;
}

std::vector<std::string> PhononVoiceSynthesisProvider::getAvailableVoices() const {
    std::lock_guard<std::mutex> lock(_settingsMutex);
    return _availableVoices;
}

bool PhononVoiceSynthesisProvider::setLanguage(const std::string& language) {
    std::string code;
    for (char c : language)
        if (!std::isspace(static_cast<unsigned char>(c))) code += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (code == "off") code = "none";
    if (code != "none" && !gradium::tts::text::parseLang(code)) return false;
    std::lock_guard<std::mutex> lock(_settingsMutex);
    _language = code;
    return true;
}

std::string PhononVoiceSynthesisProvider::getLanguage() const {
    std::lock_guard<std::mutex> lock(_settingsMutex);
    return _language;
}

bool PhononVoiceSynthesisProvider::setNormalizationRules(const std::string& rules) {
    const auto parsed = gradium::tts::text::Rules::parse(rules);
    if (!parsed) {
        spdlog::warn("[PhononTTS] Unknown normalization rules '{}'; keeping the current set.", rules);
        return false;
    }
    std::lock_guard<std::mutex> lock(_settingsMutex);
    _ruleBits = parsed->bits;
    return true;
}

void PhononVoiceSynthesisProvider::setTemperature(float temperature) {
    _temperature.store(std::max(0.0f, temperature));
}

void PhononVoiceSynthesisProvider::setStreamChunkFrames(int frames) {
    _streamChunkFrames.store(std::max(1, frames));
}

void PhononVoiceSynthesisProvider::setMaxTokensPerChunk(int tokens) {
    _maxTokensPerChunk.store(std::max(1, tokens));
}

void PhononVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    _silenceDuration = std::max(0.0f, seconds);
}

std::string PhononVoiceSynthesisProvider::voicePath(const std::string& name) const {
    const std::filesystem::path dir(_modelDir);
    if (name == "default") return (dir / "default-voice.safetensors").string();
    return (dir / "voices" / (name + ".safetensors")).string();
}

void PhononVoiceSynthesisProvider::scanVoices() {
    std::vector<std::string> voices;
    std::error_code ec;
    const std::filesystem::path dir(_modelDir);
    for (const auto& entry : std::filesystem::directory_iterator(dir / "voices", ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".safetensors")
            voices.push_back(entry.path().stem().string());
    }
    if (std::filesystem::is_regular_file(dir / "default-voice.safetensors", ec))
        voices.push_back("default");
    std::sort(voices.begin(), voices.end());

    std::lock_guard<std::mutex> lock(_settingsMutex);
    _availableVoices = std::move(voices);
}

bool PhononVoiceSynthesisProvider::applyPendingVoice() {
    std::string voice;
    {
        std::lock_guard<std::mutex> lock(_settingsMutex);
        voice = _voice;
    }
    if (voice == _loadedVoice) return true;
    try {
        _engine->setVoice(voicePath(voice));
        _loadedVoice = voice;
        return true;
    }
    catch (const std::exception& e) {
        spdlog::warn("[PhononTTS] Could not load voice '{}': {}; keeping '{}'.",
            voice, e.what(), _loadedVoice);
        std::lock_guard<std::mutex> lock(_settingsMutex);
        _voice = _loadedVoice;
        return false;
    }
}

// ---------------------------------------------------------------------------
// Worker thread (synthesis queue → synthesizeOne)
// Mirrors PocketVoiceSynthesisProvider::workerLoop.
// ---------------------------------------------------------------------------

void PhononVoiceSynthesisProvider::workerLoop() {
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

        VASDK_CTRACK_NAME("PhononTTS::processChunk");
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

        if (shouldFireCompleted && _silenceDuration > 0.0f && _audioConfig.sampleRate > 0) {
            std::vector<float> silence(
                static_cast<size_t>(_silenceDuration * _audioConfig.sampleRate), 0.0f);
            deliverAudio(silence.data(), silence.size());
        }

        if (shouldFireCompleted && _resultListener)
            _resultListener->onSynthesisCompleted();
    }
}

// ---------------------------------------------------------------------------
// synthesizeOne - one engine stream; audio arrives through StreamListener
// ---------------------------------------------------------------------------

bool PhononVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    VASDK_CTRACK_NAME("PhononTTS::synthesize");
    applyPendingVoice();
    resetResampler();

    static std::atomic<int> seed{42};
    gradium::tts::SynthOpts opts;
    opts.temperature         = _temperature.load();
    opts.duration_delta      = _durationDelta.load();
    opts.stream_chunk_frames = _streamChunkFrames.load();
    opts.seed                = seed.fetch_add(1);

    // Plan the text as Gradium's runtime does (parity-tested ports in impl/): spell numbers,
    // amounts, symbols and addresses out (normalize.cpp), then group whole sentences into
    // pieces of up to 50 tokens and cut any sentence over 200 (chunk.cpp). The model stops
    // early and drops words when prompted with much more than it learnt to say in one go.
    std::string language;
    uint32_t ruleBits;
    {
        std::lock_guard<std::mutex> lock(_settingsMutex);
        language = _language;
        ruleBits = _ruleBits;
    }
    namespace ptext = gradium::tts::text;
    std::vector<ptext::Chunk> chunks;
    try {
        chunks = ptext::planChunks(_engine->tokenizer(), text, ptext::parseLang(language),
                                   ptext::Rules{ruleBits}, _engine->frameRate(),
                                   static_cast<size_t>(_maxTokensPerChunk.load()));
    } catch (const std::invalid_argument& e) {
        // Nothing speakable once normalized (an emoji, stray punctuation): not an error.
        spdlog::debug("[PhononTTS] Skipping \"{}\": {}", text, e.what());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[PhononTTS] Text planning failed: {}", e.what());
        if (_resultListener) _resultListener->onSynthesisError(e.what());
        return false;
    }

    // Each chunk is generated on its own from the voice-primed state; their audio is
    // contiguous, so the resampler carries across them.
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (_cancelSynth.load()) return false;
        spdlog::debug("[PhononTTS] Chunk {}/{} ({} tokens, {}): \"{}\"", i + 1, chunks.size(),
                      chunks[i].tokens.size(), language, chunks[i].text);
        StreamListener listener(this);
        const auto id = _engine->synthesizeAsync(chunks[i].text, opts, listener);
        _activeStream.store(id);
        if (_cancelSynth.load()) _engine->cancelStream(id);  // cancelled before the id was visible
        _engine->waitForStream(id);
        _activeStream.store(0);

        if (listener.failed) {
            spdlog::error("[PhononTTS] Synthesis failed: {}", listener.error);
            if (_resultListener) _resultListener->onSynthesisError(listener.error);
            return false;
        }
        opts.seed = seed.fetch_add(1);
    }
    if (_cancelSynth.load()) return false;

    flushResampler();
    return true;
}

void PhononVoiceSynthesisProvider::onModelAudio(const float* pcm, size_t n) {
    if (!_resultListener || n == 0) return;
    if (_audioConfig.sampleRate <= 0 || _audioConfig.sampleRate == kModelSampleRate) {
        deliverAudio(pcm, n);
        return;
    }
    resampleStream(pcm, n, _resampleBuf);
    deliverAudio(_resampleBuf.data(), _resampleBuf.size());
}

// ---------------------------------------------------------------------------
// Streaming resampler. Audio arrives in chunks of whole 80 ms frames; resampling each
// chunk on its own (as the batch Lanczos in PocketTTS does) would leave a discontinuity
// at every chunk edge, so the input tail is carried over.
// ---------------------------------------------------------------------------

void PhononVoiceSynthesisProvider::resetResampler() {
    _rsHistory.clear();
    _rsPos = 0.0;
    if (_audioConfig.sampleRate <= 0 || _audioConfig.sampleRate == kModelSampleRate) return;
    // Zero left context so the first output sample is centred on input sample 0.
    const int R = kernelRadius(kModelSampleRate, _audioConfig.sampleRate);
    _rsHistory.assign(static_cast<size_t>(R), 0.0f);
    _rsPos = R;
}

void PhononVoiceSynthesisProvider::flushResampler() {
    if (_audioConfig.sampleRate <= 0 || _audioConfig.sampleRate == kModelSampleRate) return;
    // Zero right context lets the last real input samples be centred in the kernel.
    const int R = kernelRadius(kModelSampleRate, _audioConfig.sampleRate);
    std::vector<float> tail(static_cast<size_t>(R), 0.0f);
    resampleStream(tail.data(), tail.size(), _resampleBuf);
    deliverAudio(_resampleBuf.data(), _resampleBuf.size());
    resetResampler();
}

void PhononVoiceSynthesisProvider::resampleStream(const float* in, size_t n, std::vector<float>& out) {
    const int    dstRate     = _audioConfig.sampleRate;
    const double ratio       = static_cast<double>(dstRate) / kModelSampleRate;
    const double step        = 1.0 / ratio;
    const double filterScale = std::min(1.0, ratio);
    const int    R           = kernelRadius(kModelSampleRate, dstRate);

    _rsHistory.insert(_rsHistory.end(), in, in + n);
    out.clear();

    const long long histLen = static_cast<long long>(_rsHistory.size());
    while (static_cast<long long>(std::floor(_rsPos)) + R < histLen) {
        const long long base  = static_cast<long long>(std::floor(_rsPos));
        double sample = 0.0, wSum = 0.0;
        for (long long j = base - R + 1; j <= base + R; ++j) {
            double x = (_rsPos - j) * filterScale;
            double w = (std::abs(x) < kLanczosA) ? sinc(x) * sinc(x / kLanczosA) : 0.0;
            sample += _rsHistory[static_cast<size_t>(j)] * w;
            wSum += w;
        }
        out.push_back((wSum > 0.0) ? static_cast<float>(sample / wSum) : 0.0f);
        _rsPos += step;
    }

    // Keep R samples of left context for the next call.
    const long long drop = static_cast<long long>(std::floor(_rsPos)) - R;
    if (drop > 0) {
        _rsHistory.erase(_rsHistory.begin(), _rsHistory.begin() + static_cast<std::ptrdiff_t>(drop));
        _rsPos -= static_cast<double>(drop);
    }
}

// ---------------------------------------------------------------------------
// deliverAudio - format convert + call listener
// ---------------------------------------------------------------------------

void PhononVoiceSynthesisProvider::deliverAudio(const float* pcm, size_t n) {
    if (!_resultListener || n == 0) return;

    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT: {
        _int16Buf.resize(n);
        AudioSIMD::convertFloatToInt16(pcm, _int16Buf.data(), n);
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int16Buf.data()), n * sizeof(int16_t));
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm), n * sizeof(float));
        break;
    }
    case va::audio::AudioFormat::INT: {
        _int32Buf.resize(n);
        AudioSIMD::convertFloatToInt32(pcm, _int32Buf.data(), n);
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_int32Buf.data()), n * sizeof(int32_t));
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        _uint8Buf.resize(n);
        AudioSIMD::convertFloatToUint8(pcm, _uint8Buf.data(), n);
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(_uint8Buf.data()), n * sizeof(uint8_t));
        break;
    }
    default:
        spdlog::warn("[PhononTTS] Unknown audio format; delivering as float.");
        _resultListener->onAudioDataReceived(
            reinterpret_cast<const char*>(pcm), n * sizeof(float));
        break;
    }
}
