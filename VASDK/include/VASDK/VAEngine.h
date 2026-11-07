#pragma once

#include "vasdk_export.h"
#include "CommonAudioUtils.h"
#include "engine/Session.h"

#include <memory>
#include <string>

namespace vasdk::engine { class EngineConfig; }

namespace va {

namespace engine { class ProviderRegistry; }

/**
 * @brief A complete voice assistant built from an engine config: one conversation.
 *
 * A Session with a Runtime of its own. Creates the providers named in the config (VAD, STT,
 * TTS, turn detection) from a ProviderRegistry and wires the standard pipeline; see
 * engine::Session for the pipeline, slots, controls and events. To serve several
 * conversations at once with models loaded once, use engine::Runtime and its sessions.
 *
 * @code
 * va::engine::ProviderRegistry registry;
 * va::thirdparty::registerAllProviders(registry);
 * va::VAEngine::Options options;
 * options.transport = myLocalTransport;
 * auto engine = va::VAEngine::create("engine.txtpb", registry, options, &error);
 * if (engine && engine->start(&error)) engine->say("Hi, how can I help?");
 * @endcode
 */
class VASDK_EXPORT VAEngine : public engine::Session {
public:
    struct Options : engine::Session::Options {
        /** Used when the config's models_dir is empty. */
        std::string modelsDir;
        audio::AudioConfig inputAudio{16000, audio::AudioFormat::SHORT, 1};
        audio::AudioConfig outputAudio{16000, audio::AudioFormat::SHORT, 1};
        int frameSize = 1600;   ///< samples per frame: 100 ms at 16 kHz, the VAD/turn decision grid
    };

    /** nullptr, with `error` set, when a provider cannot be created. */
    static std::unique_ptr<VAEngine> create(const vasdk::engine::EngineConfig& config,
                                            const engine::ProviderRegistry& registry,
                                            Options options, std::string* error = nullptr);
    /** Loads the config file (text proto, JSON or binary; ${ENV} expanded), then create(). */
    static std::unique_ptr<VAEngine> create(const std::string& configPath,
                                            const engine::ProviderRegistry& registry,
                                            Options options, std::string* error = nullptr);

private:
    VAEngine(std::shared_ptr<engine::Runtime> runtime, Options options, std::string* error);
};

} // namespace va
