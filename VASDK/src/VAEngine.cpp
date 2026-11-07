#include "VAEngine.h"
#include "WebRtcTransport.h"

#include "engine/ProtoFileLoader.h"
#include "engine/Runtime.h"

namespace va {

namespace {

engine::Runtime::Options runtimeOptions(const VAEngine::Options& options) {
    engine::Runtime::Options runtime;
    runtime.modelsDir = options.modelsDir;
    runtime.inputAudio = options.inputAudio;
    runtime.outputAudio = options.outputAudio;
    runtime.frameSize = options.frameSize;
    runtime.maxSessions = 1;
    return runtime;
}

} // namespace

std::unique_ptr<VAEngine> VAEngine::create(const std::string& configPath, const engine::ProviderRegistry& registry,
                                           Options options, std::string* error) {
    vasdk::engine::EngineConfig config;
    std::string loadError;
    if (!engine::loadEngineConfig(configPath, config, &loadError)) {
        if (error) *error = loadError;
        return nullptr;
    }
    return create(config, registry, std::move(options), error);
}

std::unique_ptr<VAEngine> VAEngine::create(const vasdk::engine::EngineConfig& config,
                                           const engine::ProviderRegistry& registry, Options options,
                                           std::string* error) {
    if (config.providers().has_webrtc()) {   // a browser brings the audio, not the app
        const engine::ProviderContext context{options.modelsDir, options.inputAudio, options.frameSize, nullptr};
        std::string reason;
        auto peer = registry.create<network::WebRtcInterface>(config.providers().webrtc(), context, &reason);
        if (!peer) {
            if (error) *error = "webrtc: " + reason;
            return nullptr;
        }
        options.transport = std::make_shared<transport::WebRtcTransport>(peer);
        options.sendAllFramesToTransport = true;   // the page shows the agent's state
    }
    if (!options.transport) {
        if (error) *error = "Options::transport is required";
        return nullptr;
    }
    auto runtime = engine::Runtime::create(config, registry, runtimeOptions(options), error);
    if (!runtime) return nullptr;
    std::unique_ptr<VAEngine> engine(new VAEngine(std::move(runtime), std::move(options), error));
    return engine->created() ? std::move(engine) : nullptr;
}

VAEngine::VAEngine(std::shared_ptr<engine::Runtime> runtime, Options options, std::string* error)
    : Session(std::move(runtime), std::move(options), error) {}

} // namespace va
