#include "engine/Runtime.h"

#include <algorithm>
#include "nlu/EmbeddingNluProvider.h"

#include "AudioRingBuffer.h"
#include "GenericTranscriptionProvider.h"
#include "GenericVoiceSynthesisProvider.h"
#include "TurnDetectionProvider.h"
#include "VADProvider.h"
#include "WakeWordProvider.h"
#include "engine/ProtoFileLoader.h"

#include <spdlog/spdlog.h>

namespace va {
namespace engine {

std::shared_ptr<Runtime> Runtime::create(const std::string& configPath, const ProviderRegistry& registry,
                                         Options options, std::string* error) {
    vasdk::engine::EngineConfig config;
    std::string loadError;
    if (!loadEngineConfig(configPath, config, &loadError)) {
        if (error) *error = loadError;
        return nullptr;
    }
    return create(config, registry, std::move(options), error);
}

namespace {

bool applyFormat(const vasdk::engine::AudioFormat& config, const char* name, audio::AudioConfig& format,
                 std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = std::string("audio.") + name + "." + message;
        return false;
    };
    if (config.has_sample_rate()) {
        if (config.sample_rate() < 8000 || config.sample_rate() > 192000)
            return fail("sample_rate: " + std::to_string(config.sample_rate()) + " Hz is outside 8000-192000");
        format.sampleRate = config.sample_rate();
    }
    if (config.has_channels()) {
        if (config.channels() < 1 || config.channels() > 8)
            return fail("channels: " + std::to_string(config.channels()) + " is outside 1-8");
        format.channels = config.channels();
    }
    if (config.has_encoding()) {
        switch (config.encoding()) {
        case vasdk::engine::AudioFormat::PCM_8: format.format = audio::AudioFormat::BYTE; break;
        case vasdk::engine::AudioFormat::PCM_32: format.format = audio::AudioFormat::INT; break;
        case vasdk::engine::AudioFormat::FLOAT_32: format.format = audio::AudioFormat::FLOAT; break;
        default: format.format = audio::AudioFormat::SHORT; break;
        }
    }
    return true;
}

} // namespace

bool applyAudioConfig(const vasdk::engine::Audio& config, audio::AudioConfig& inputAudio,
                      audio::AudioConfig& outputAudio, int& frameSize, std::string* error) {
    const int inputRate = inputAudio.sampleRate;
    const int frameMs = inputRate > 0 ? static_cast<int>(static_cast<int64_t>(frameSize) * 1000 / inputRate) : 0;
    if (!applyFormat(config.input(), "input", inputAudio, error)) return false;
    if (!applyFormat(config.output(), "output", outputAudio, error)) return false;
    if (config.has_frame_ms()) {
        if (config.frame_ms() < 10 || config.frame_ms() > 1000) {
            if (error) *error = "audio.frame_ms: " + std::to_string(config.frame_ms()) + " is outside 10-1000";
            return false;
        }
        frameSize = static_cast<int>(static_cast<int64_t>(inputAudio.sampleRate) * config.frame_ms() / 1000);
    } else if (inputAudio.sampleRate != inputRate && frameMs > 0) {
        frameSize = static_cast<int>(static_cast<int64_t>(inputAudio.sampleRate) * frameMs / 1000);  // same duration
    }
    return true;
}

std::shared_ptr<Runtime> Runtime::create(const vasdk::engine::EngineConfig& config, const ProviderRegistry& registry,
                                         Options options, std::string* error) {
    if (!applyAudioConfig(config.audio(), options.inputAudio, options.outputAudio, options.frameSize, error))
        return nullptr;
    std::shared_ptr<Runtime> runtime(new Runtime(config, registry, std::move(options)));
    auto first = runtime->createProviderSet(error);
    if (!first) return nullptr;
    runtime->_idle.push_back(std::move(first));
    if (const auto& encoder = config.providers().sentence_encoder(); !encoder.id().empty()) {   // one for every session
        std::string reason;
        const ProviderContext context{runtime->_options.modelsDir, runtime->_options.inputAudio, runtime->_options.frameSize, nullptr};
        runtime->_sentenceEncoder = runtime->_registry.create<nlu::SentenceEncoder>(encoder, context, &reason);
        if (!runtime->_sentenceEncoder) {
            if (error) *error = "Sentence encoder: " + reason;
            return nullptr;
        }
    }
    return runtime;
}

Runtime::Runtime(vasdk::engine::EngineConfig config, ProviderRegistry registry, Options options)
    : _config(std::move(config)), _registry(std::move(registry)), _options(std::move(options)) {
    if (!_config.models_dir().empty()) _options.modelsDir = _config.models_dir();
}

std::shared_ptr<Runtime::ProviderSet> Runtime::createProviderSet(std::string* error) const {
    auto fail = [error](const std::string& message) -> std::shared_ptr<ProviderSet> {
        if (error) *error = message;
        return nullptr;
    };
    auto set = std::make_shared<ProviderSet>();
    set->ringBuffer = std::make_shared<AudioRingBuffer>(
        (long)_options.inputAudio.sampleRate * 90, _options.inputAudio.getSampleSizeInBytes());
    const ProviderContext input{_options.modelsDir, _options.inputAudio, _options.frameSize, set->ringBuffer};
    const ProviderContext output{_options.modelsDir, _options.outputAudio, _options.frameSize, set->ringBuffer};
    const auto& providers = _config.providers();
    std::string reason;

    set->vad = _registry.create<audio::VADProvider>(providers.vad(), input, &reason);
    if (!set->vad) return fail("VAD: " + reason);
    set->stt = _registry.create<audio::GenericTranscriptionProvider>(providers.stt(), input, &reason, &set->sttId);
    if (!set->stt) return fail("STT: " + reason);
    set->tts = _registry.create<audio::GenericVoiceSynthesisProvider>(providers.tts(), output, &reason, &set->ttsId);
    if (!set->tts) return fail("TTS: " + reason);
    // Turn detection is optional: with no model built in, the VAD's silence ends the turn.
    if (!providers.turn().id().empty() || !_registry.ids<audio::TurnDetectionProvider>().empty()) {
        set->turn = _registry.create<audio::TurnDetectionProvider>(providers.turn(), input, &reason);
        if (!set->turn) return fail("Turn detection: " + reason);
    }
    const auto mode = _config.activation().mode();
    if (mode == vasdk::engine::Activation::WAKE_WORD || mode == vasdk::engine::Activation::HYBRID) {
        set->wakeWord = _registry.create<audio::WakeWordProvider>(providers.wake_word(), input, &reason);
        if (!set->wakeWord) return fail("Wake word: " + reason);
        if (!set->wakeWord->initialize(_options.inputAudio)) return fail("Wake word: " + set->wakeWord->getName() + " failed to start");
    }
    return set;
}

namespace {
// The spec's fallbacks after `started`: what to try when it stops working.
vasdk::engine::ProviderSpec fallbacksAfter(const vasdk::engine::ProviderSpec& spec, const std::string& started) {
    vasdk::engine::ProviderSpec rest;
    *rest.mutable_fallback_options() = spec.fallback_options();
    const auto& fallback = spec.fallback();
    auto next = std::find(fallback.begin(), fallback.end(), started);
    next = next == fallback.end() ? fallback.begin() : next + 1;
    for (; next != fallback.end(); ++next) {
        if (*next == started) continue;
        if (!rest.id().empty()) {
            rest.add_fallback(*next);
            continue;
        }
        rest.set_id(*next);
        if (const auto options = spec.fallback_options().find(*next); options != spec.fallback_options().end())
            *rest.mutable_options() = options->second;
    }
    return rest;
}
} // namespace

bool Runtime::loadFallbacks(ProviderSet& set, std::string* error) const {
    const ProviderContext input{_options.modelsDir, _options.inputAudio, _options.frameSize, set.ringBuffer};
    const ProviderContext output{_options.modelsDir, _options.outputAudio, _options.frameSize, set.ringBuffer};
    std::string reasons;
    if (!set.sttFallback) {
        const auto spec = fallbacksAfter(_config.providers().stt(), set.sttId);
        std::string reason;
        if (!spec.id().empty())
            set.sttFallback = _registry.create<audio::GenericTranscriptionProvider>(spec, input, &reason, &set.sttFallbackId);
        if (!set.sttFallback) reasons += "STT: " + (reason.empty() ? std::string("no fallback") : reason);
    }
    if (!set.ttsFallback) {
        const auto spec = fallbacksAfter(_config.providers().tts(), set.ttsId);
        std::string reason;
        if (!spec.id().empty())
            set.ttsFallback = _registry.create<audio::GenericVoiceSynthesisProvider>(spec, output, &reason, &set.ttsFallbackId);
        if (!set.ttsFallback) reasons += (reasons.empty() ? "" : "; ") + ("TTS: " + (reason.empty() ? std::string("no fallback") : reason));
    }
    if (error) *error = reasons;
    return set.sttFallback || set.ttsFallback;
}

std::unique_ptr<Session> Runtime::createSession(Session::Options options, std::string* error) {
    std::unique_ptr<Session> session(new Session(shared_from_this(), std::move(options), error));
    return session->created() ? std::move(session) : nullptr;
}

std::shared_ptr<Runtime::ProviderSet> Runtime::acquire(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_options.maxSessions > 0 && _active >= _options.maxSessions) {
            if (error) *error = "all " + std::to_string(_options.maxSessions) + " sessions are in use";
            return nullptr;
        }
        ++_active;
        if (!_idle.empty()) {
            auto set = std::move(_idle.back());
            _idle.pop_back();
            return set;
        }
    }
    // Loading models takes a while: not under the lock.
    spdlog::info("[Runtime] Loading providers for another concurrent session");
    auto set = createProviderSet(error);
    if (!set) {
        std::lock_guard<std::mutex> lock(_mutex);
        --_active;
    }
    return set;
}

void Runtime::release(std::shared_ptr<ProviderSet> providers) {
    std::lock_guard<std::mutex> lock(_mutex);
    --_active;
    _idle.push_back(std::move(providers));
}

size_t Runtime::activeSessions() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _active;
}

size_t Runtime::idleProviderSets() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _idle.size();
}

} // namespace engine
} // namespace va
