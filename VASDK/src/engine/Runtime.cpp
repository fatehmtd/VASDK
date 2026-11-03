#include "engine/Runtime.h"

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

std::shared_ptr<Runtime> Runtime::create(const vasdk::engine::EngineConfig& config, const ProviderRegistry& registry,
                                         Options options, std::string* error) {
    std::shared_ptr<Runtime> runtime(new Runtime(config, registry, std::move(options)));
    auto first = runtime->createProviderSet(error);
    if (!first) return nullptr;
    runtime->_idle.push_back(std::move(first));
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
    set->stt = _registry.create<audio::GenericTranscriptionProvider>(providers.stt(), input, &reason);
    if (!set->stt) return fail("STT: " + reason);
    set->tts = _registry.create<audio::GenericVoiceSynthesisProvider>(providers.tts(), output, &reason);
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
