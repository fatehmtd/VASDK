#pragma once

#include "vasdk_export.h"
#include "AgentStateProcessor.h"
#include "CommonAudioUtils.h"
#include "metrics/MetricsConfig.h"

#include <memory>
#include <string>

namespace vasdk::engine { class EngineConfig; }

namespace va {

class AudioRingBuffer;
namespace engine { class ProviderRegistry; }
namespace llm { class LLMTransport; class Tool; }
namespace metrics { class MetricsReporter; }
namespace pipeline { class FrameProcessor; class Pipeline; }
namespace transport { class AudioTransport; }

/**
 * @brief A complete voice assistant built from an engine config.
 *
 * Creates the providers named in the config (VAD, STT, TTS, turn detection) from a
 * ProviderRegistry and wires the standard pipeline:
 *
 *   input → trigger → [AfterInput] → VAD → turn → STT → [AfterTranscription]
 *     → agent state → LLM → tools → [BeforeSynthesis] → TTS → [BeforeOutput] → output
 *
 * The LLM stages are there only when the config has an `llm` endpoint (or
 * Options::llmTransport is set); without them the engine still speaks say() text.
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
class VASDK_EXPORT VAEngine {
public:
    /** Where addProcessor() puts an app processor in the standard pipeline. */
    enum class Slot {
        AfterInput,           ///< raw mic audio, before VAD (recorders, visualisers)
        AfterTranscription,   ///< transcriptions, before the agent reacts (speaker id, logging)
        BeforeSynthesis,      ///< reply text, before TTS
        BeforeOutput,         ///< synthesized audio, before the transport plays it
    };

    struct Options {
        /** Required: mic in, speech out. VAEngine configures, starts and stops it. */
        std::shared_ptr<transport::AudioTransport> transport;
        /** Used when the config's models_dir is empty. */
        std::string modelsDir;
        std::string systemPrompt = "You are a helpful voice assistant. Keep responses brief and conversational.";
        /** Appended to every LLM request: how the reply must be formatted. */
        std::string responseFormatInstruction;
        /** Use this LLM transport (a local model, say) instead of the config's `llm` endpoint. */
        std::shared_ptr<llm::LLMTransport> llmTransport;
        /** Send every frame to the transport, not just speech audio: for a UI that shows state (WebRTC page). */
        bool sendAllFramesToTransport = false;
        pipeline::BackchannelConfig backchannel;
        audio::AudioConfig inputAudio{16000, audio::AudioFormat::SHORT, 1};
        audio::AudioConfig outputAudio{16000, audio::AudioFormat::SHORT, 1};
        int frameSize = 1600;   ///< samples per frame: 100 ms at 16 kHz, the VAD/turn decision grid
        /** Optional: bind every processor's metrics to this reporter. */
        std::shared_ptr<metrics::MetricsReporter> metricsReporter;
        metrics::MetricsConfig metricsConfig;
    };

    /** nullptr, with `error` set, when a provider cannot be created. */
    static std::unique_ptr<VAEngine> create(const vasdk::engine::EngineConfig& config,
                                            const engine::ProviderRegistry& registry,
                                            Options options, std::string* error = nullptr);
    /** Loads the config file (text proto, JSON or binary; ${ENV} expanded), then create(). */
    static std::unique_ptr<VAEngine> create(const std::string& configPath,
                                            const engine::ProviderRegistry& registry,
                                            Options options, std::string* error = nullptr);
    ~VAEngine();
    VAEngine(const VAEngine&) = delete;
    VAEngine& operator=(const VAEngine&) = delete;

    /** Adds an app processor at `slot`, after any added there before. Before start() only. */
    bool addProcessor(Slot slot, std::shared_ptr<pipeline::FrameProcessor> processor);
    /** Makes a tool available to the LLM. Can be called before start(). */
    void registerTool(const llm::Tool& tool);

    /** Starts the transport and the pipeline. */
    bool start(std::string* error = nullptr);
    void stop();

    /** Speaks `text` as the assistant. Thread-safe. */
    void say(const std::string& text);
    /** Sends `text` as a finished user turn, as if spoken. Needs an LLM. Thread-safe. */
    void injectUserText(const std::string& text);
    /** Stops the assistant's current reply, like a barge-in. Thread-safe. */
    void interrupt();

    bool hasLLM() const;
    /** "TransportInputProcessor → TriggerProcessor → VADFrameProcessor (SileroVAD) → ..." */
    std::string describe() const;

    /** Mic audio, for app processors that read it directly (recorders, speaker id). */
    std::shared_ptr<AudioRingBuffer> inputRingBuffer() const;
    std::shared_ptr<llm::LLMTransport> llmTransport() const;
    /** nullptr before start(). */
    std::shared_ptr<pipeline::Pipeline> pipeline() const;

private:
    struct State;
    explicit VAEngine(std::unique_ptr<State> state);
    std::unique_ptr<State> _state;
};

} // namespace va
