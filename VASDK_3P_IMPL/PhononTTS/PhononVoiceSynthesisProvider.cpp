#include "PhononVoiceSynthesisProvider.hpp"

#include "impl/phonon.h"
#include "impl/normalize.h"
#include "impl/chunk.h"

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
        if (_owner->cancelled()) return;
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
    : QueuedVoiceSynthesisProvider("PhononTTS", Delivery::Direct),
      _modelDir(modelDir), _voice(voiceName),
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

bool PhononVoiceSynthesisProvider::loadEngine() {
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
    return true;
}

void PhononVoiceSynthesisProvider::onCancel() {
    // Generation stops at the next frame boundary; a stale id is a no-op in the engine.
    const uint64_t id = _activeStream.load();
    if (id != 0 && _engine) _engine->cancelStream(id);
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
// The silence after a sequence said in full
// ---------------------------------------------------------------------------

void PhononVoiceSynthesisProvider::onSequenceCompleted() {
    if (_silenceDuration <= 0.0f || audioConfig().sampleRate <= 0) return;
    std::vector<float> silence(static_cast<size_t>(_silenceDuration * audioConfig().sampleRate), 0.0f);
    deliverAudio(silence.data(), silence.size());
}

// ---------------------------------------------------------------------------
// synthesizeOne - one engine stream; audio arrives through StreamListener
// ---------------------------------------------------------------------------

bool PhononVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    VASDK_CTRACK_NAME("PhononTTS::synthesize");
    applyPendingVoice();
    _resampler.reset(modelSampleRate(), audioConfig().sampleRate);

    gradium::tts::SynthOpts opts;
    opts.temperature         = _temperature.load();
    opts.duration_delta      = _durationDelta.load();
    opts.stream_chunk_frames = _streamChunkFrames.load();
    opts.seed                = _seed.fetch_add(1);

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
        reportError(std::string("text planning: ") + e.what());
        return false;
    }

    // Each chunk is generated on its own from the voice-primed state; their audio is
    // contiguous, so the resampler carries across them.
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (cancelled()) return false;
        spdlog::debug("[PhononTTS] Chunk {}/{} ({} tokens, {}): \"{}\"", i + 1, chunks.size(),
                      chunks[i].tokens.size(), language, chunks[i].text);
        StreamListener listener(this);
        const auto id = _engine->synthesizeAsync(chunks[i].text, opts, listener);
        _activeStream.store(id);
        if (cancelled()) _engine->cancelStream(id);  // cancelled before the id was visible
        _engine->waitForStream(id);
        _activeStream.store(0);

        if (listener.failed) {
            reportError(listener.error);
            return false;
        }
        opts.seed = _seed.fetch_add(1);
    }
    if (cancelled()) return false;

    flushResampler();
    return true;
}

void PhononVoiceSynthesisProvider::onModelAudio(const float* pcm, size_t n) {
    if (!listener() || n == 0) return;
    if (_resampler.passthrough()) {
        deliverAudio(pcm, n);
        return;
    }
    _resampleBuf.clear();
    _resampler.process(pcm, n, _resampleBuf);
    deliverAudio(_resampleBuf.data(), _resampleBuf.size());
}

void PhononVoiceSynthesisProvider::flushResampler() {
    // The last real samples, centred in the kernel.
    _resampleBuf.clear();
    _resampler.flush(_resampleBuf);
    deliverAudio(_resampleBuf.data(), _resampleBuf.size());
}
