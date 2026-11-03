#pragma once

#include "vasdk_export.h"
#include "../CommonAudioUtils.h"
#include "ProviderRegistry.h"
#include "Session.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va {

class AudioRingBuffer;
namespace audio {
class VADProvider;
class GenericTranscriptionProvider;
class GenericVoiceSynthesisProvider;
class TurnDetectionProvider;
}

namespace engine {

/**
 * @brief What many conversations share: the engine config and the providers it names.
 *
 * Create one per process, then one Session per conversation. A session takes a set of
 * providers (VAD, STT, TTS, turn model) for as long as it runs; when it ends, the set goes back
 * here and the next session reuses it. Models therefore load once per conversation running at
 * the same time, not once per conversation.
 *
 * @code
 * auto runtime = va::engine::Runtime::create(config, registry, {}, &error);
 * va::engine::Session::Options call;
 * call.id = "call-42";
 * call.transport = callTransport;
 * auto session = runtime->createSession(call, &error);
 * session->start();
 * @endcode
 */
class VASDK_EXPORT Runtime : public std::enable_shared_from_this<Runtime> {
public:
    struct Options {
        /** Used when the config's models_dir is empty. */
        std::string modelsDir;
        audio::AudioConfig inputAudio{16000, audio::AudioFormat::SHORT, 1};
        audio::AudioConfig outputAudio{16000, audio::AudioFormat::SHORT, 1};
        int frameSize = 1600;     ///< samples per frame: 100 ms at 16 kHz, the VAD/turn decision grid
        size_t maxSessions = 0;   ///< sessions running at once; 0 = no limit
    };

    /** One session's providers, with the mic ring buffer the turn model reads. */
    struct ProviderSet {
        std::shared_ptr<AudioRingBuffer> ringBuffer;
        std::shared_ptr<audio::VADProvider> vad;
        std::shared_ptr<audio::GenericTranscriptionProvider> stt;
        std::shared_ptr<audio::GenericVoiceSynthesisProvider> tts;
        std::shared_ptr<audio::TurnDetectionProvider> turn;   ///< null: the VAD's silence ends the turn
    };

    /**
     * Creates the first set of providers right away, so a misconfigured provider fails here.
     * nullptr, with `error` set ("STT: parakeet: not built into this binary"), on failure.
     */
    static std::shared_ptr<Runtime> create(const vasdk::engine::EngineConfig& config, const ProviderRegistry& registry,
                                           Options options, std::string* error = nullptr);
    /** Loads the config file (text proto, JSON or binary; ${ENV} expanded), then create(). */
    static std::shared_ptr<Runtime> create(const std::string& configPath, const ProviderRegistry& registry,
                                           Options options, std::string* error = nullptr);

    /** A new conversation; nullptr, with `error` set, at maxSessions or if a provider fails. */
    std::unique_ptr<Session> createSession(Session::Options options, std::string* error = nullptr);

    const vasdk::engine::EngineConfig& config() const { return _config; }
    const Options& options() const { return _options; }
    size_t activeSessions() const;
    /** Provider sets loaded and waiting for a session. */
    size_t idleProviderSets() const;

    /** For Session: a set to use until release(). */
    std::shared_ptr<ProviderSet> acquire(std::string* error);
    void release(std::shared_ptr<ProviderSet> providers);

private:
    Runtime(vasdk::engine::EngineConfig config, ProviderRegistry registry, Options options);
    std::shared_ptr<ProviderSet> createProviderSet(std::string* error) const;

    vasdk::engine::EngineConfig _config;
    ProviderRegistry _registry;
    Options _options;
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<ProviderSet>> _idle;
    size_t _active = 0;
};

} // namespace engine
} // namespace va
