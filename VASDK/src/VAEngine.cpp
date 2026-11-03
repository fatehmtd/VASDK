#include "VAEngine.h"

#include "AudioRingBuffer.h"
#include "Pipeline.h"
#include "TTSFrameProcessor.h"
#include "TranscriptionProcessor.h"
#include "TransportInputProcessor.h"
#include "TransportOutputProcessor.h"
#include "TriggerProcessor.h"
#include "TurnDetectionProcessor.h"
#include "VADFrameProcessor.h"
#include "engine/ProtoFileLoader.h"
#include "engine/ProviderRegistry.h"
#include "llm/LLMFirePolicy.h"
#include "llm/LLMProcessor.h"
#include "llm/OpenAITransport.h"
#include "llm/PromptTemplate.h"
#include "llm/ToolDispatchProcessor.h"
#include "metrics/MetricsBinder.h"
#include "metrics/MetricsReporter.h"

#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <vector>

namespace va {

namespace {

using pipeline::FlowDirection;
using pipeline::FrameProcessor;

// Entry point for frames that come from the app rather than the mic: say(), injectUserText().
class FrameInjector : public FrameProcessor {
public:
    explicit FrameInjector(std::string name) : _name(std::move(name)) {}
    std::string getName() const override { return _name; }
    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override {
        if (direction == FlowDirection::DOWNSTREAM) pushDownstream(frame);
        else pushUpstream(frame);
    }
    /** Thread-safe: queued, then pushed downstream on this processor's thread. */
    void inject(std::shared_ptr<transport::Frame> frame) { enqueueFrame(std::move(frame), FlowDirection::DOWNSTREAM); }

private:
    std::string _name;
};

// On a hard barge-in, drops the audio the STT buffered while the assistant was talking, so the
// echo doesn't poison the next final. Not with back-channel handling on: the overlap is decided
// on the STT result, so the buffer must keep the whole segment.
class BargeInHandler : public FrameProcessor {
public:
    explicit BargeInHandler(std::shared_ptr<audio::GenericTranscriptionProvider> transcription)
        : _transcription(std::move(transcription)) {}
    std::string getName() const override { return "BargeInHandler"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override {
        return frame->getType() == transport::FrameType::AGENT_STATE;
    }
    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override {
        if (direction == FlowDirection::DOWNSTREAM) {
            auto agentFrame = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame);
            if (agentFrame && agentFrame->transition == transport::AgentStateTransition::BARGE_IN_INTERRUPT) {
                _transcription->clearAudioBuffer();
                spdlog::info("[BargeInHandler] Cleared transcription buffer");
            }
        }
        pushDownstream(frame);
    }

private:
    std::shared_ptr<audio::GenericTranscriptionProvider> _transcription;
};

} // namespace

struct VAEngine::State {
    Options options;
    std::shared_ptr<AudioRingBuffer> ringBuffer;
    std::shared_ptr<audio::GenericTranscriptionProvider> transcriptionProvider;

    std::shared_ptr<pipeline::TransportInputProcessor> transportInput;
    std::shared_ptr<pipeline::TriggerProcessor> trigger;
    std::shared_ptr<pipeline::VADFrameProcessor> vad;
    std::shared_ptr<pipeline::TurnDetectionProcessor> turnDetection;
    std::shared_ptr<pipeline::TranscriptionFrameProcessor> transcription;
    std::shared_ptr<pipeline::AgentStateProcessor> agentState;
    std::shared_ptr<FrameProcessor> bargeInHandler;   // only with back-channel handling off
    std::shared_ptr<FrameInjector> userInput;
    std::shared_ptr<llm::LLMTransport> llmTransport;
    std::shared_ptr<llm::LLMProcessor> llm;
    std::shared_ptr<llm::ToolDispatchProcessor> tools = std::make_shared<llm::ToolDispatchProcessor>();
    std::shared_ptr<FrameInjector> speech;
    std::shared_ptr<pipeline::TTSFrameProcessor> tts;
    std::shared_ptr<pipeline::TransportOutputProcessor> transportOutput;

    std::map<Slot, std::vector<std::shared_ptr<FrameProcessor>>> slots;
    std::shared_ptr<pipeline::Pipeline> pipeline;

    std::vector<std::shared_ptr<FrameProcessor>> chain() const {
        std::vector<std::shared_ptr<FrameProcessor>> processors{transportInput, trigger};
        auto addSlot = [&](Slot slot) {
            auto found = slots.find(slot);
            if (found != slots.end()) processors.insert(processors.end(), found->second.begin(), found->second.end());
        };
        addSlot(Slot::AfterInput);
        processors.insert(processors.end(), {vad, turnDetection, transcription});
        addSlot(Slot::AfterTranscription);
        processors.push_back(agentState);
        if (bargeInHandler) processors.push_back(bargeInHandler);
        if (llm) processors.insert(processors.end(), {userInput, llm, tools});
        addSlot(Slot::BeforeSynthesis);
        processors.insert(processors.end(), {speech, tts});
        addSlot(Slot::BeforeOutput);
        processors.push_back(transportOutput);
        return processors;
    }
};

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
    auto fail = [error](const std::string& message) -> std::unique_ptr<VAEngine> {
        if (error) *error = message;
        return nullptr;
    };
    if (!options.transport) return fail("Options::transport is required");

    auto state = std::make_unique<State>();
    state->ringBuffer = std::make_shared<AudioRingBuffer>(
        (long)options.inputAudio.sampleRate * 90, options.inputAudio.getSampleSizeInBytes());
    auto& transport = *options.transport;
    transport.setInputAudioConfig(options.inputAudio);
    transport.setOutputAudioConfig(options.outputAudio);
    transport.setFrameSize(options.frameSize);
    transport.setRingBuffer(state->ringBuffer);
    if (options.metricsReporter) transport.setMetricsConfig(options.metricsConfig.transport);

    //  Providers
    const std::string modelsDir = config.models_dir().empty() ? options.modelsDir : config.models_dir();
    const engine::ProviderContext input{modelsDir, options.inputAudio, options.frameSize, state->ringBuffer};
    const engine::ProviderContext output{modelsDir, options.outputAudio, options.frameSize, state->ringBuffer};
    const auto& providers = config.providers();
    std::string providerError;

    auto vadProvider = registry.create<audio::VADProvider>(providers.vad(), input, &providerError);
    if (!vadProvider) return fail("VAD: " + providerError);
    state->transcriptionProvider = registry.create<audio::GenericTranscriptionProvider>(
        providers.stt(), input, &providerError);
    if (!state->transcriptionProvider) return fail("STT: " + providerError);
    auto ttsProvider = registry.create<audio::GenericVoiceSynthesisProvider>(providers.tts(), output, &providerError);
    if (!ttsProvider) return fail("TTS: " + providerError);
    // Turn detection is optional: with no model built in, the VAD's silence ends the turn.
    std::shared_ptr<audio::TurnDetectionProvider> turnProvider;
    if (!providers.turn().id().empty() || !registry.ids<audio::TurnDetectionProvider>().empty()) {
        turnProvider = registry.create<audio::TurnDetectionProvider>(providers.turn(), input, &providerError);
        if (!turnProvider) return fail("Turn detection: " + providerError);
    }

    //  Processors
    state->transportInput = std::make_shared<pipeline::TransportInputProcessor>();
    state->transportInput->addTransport(options.transport);
    state->trigger = std::make_shared<pipeline::TriggerProcessor>();

    state->vad = std::make_shared<pipeline::VADFrameProcessor>(vadProvider, 3.0f);
    state->vad->setMinFrameSize(options.frameSize);
    pipeline::VADConfig vadConfig;
    vadConfig.vadThresholdMs           = 250;
    vadConfig.minSpeechDurationMs      = 250;
    vadConfig.maxSpeechPauseDurationMs = 500;
    vadConfig.maxSpeechEndDurationMs   = 250;
    state->vad->setConfig(vadConfig);
    state->vad->setRingBuffer(state->ringBuffer);

    state->turnDetection = std::make_shared<pipeline::TurnDetectionProcessor>(turnProvider);

    state->transcription = std::make_shared<pipeline::TranscriptionFrameProcessor>(state->transcriptionProvider);
    state->transcription->configSilenceBehavior(false, false, 1000);
    state->transcription->setStreamingMode(false);
    state->transcription->setRingBuffer(state->ringBuffer);

    state->agentState = std::make_shared<pipeline::AgentStateProcessor>();
    state->agentState->setAlwaysOn(true);
    state->agentState->setBackchannelConfig(options.backchannel);
    if (!options.backchannel.enabled)
        state->bargeInHandler = std::make_shared<BargeInHandler>(state->transcriptionProvider);

    state->tts = std::make_shared<pipeline::TTSFrameProcessor>(ttsProvider);
    state->tts->setPlaybackFlush([transport = options.transport] { transport->flushOutputAudio(); });
    state->tts->setPlaybackHold([transport = options.transport](transport::PendingPlaybackMode mode, bool held,
                                                               float duckGainDecibels) {
        transport->holdOutputAudio(mode, held, duckGainDecibels);
    });
    state->tts->setPendingPlayback(options.backchannel.pendingPlaybackMode, options.backchannel.duckGainDecibels,
                                   options.backchannel.duckDurationMs);
    state->agentState->setPlaybackRemainingMsProvider(
        [tts = std::weak_ptr<pipeline::TTSFrameProcessor>(state->tts)] {
            auto ttsProcessor = tts.lock();
            return ttsProcessor ? ttsProcessor->getRemainingReplyPlaybackMs() : -1;
        });
    state->speech = std::make_shared<FrameInjector>("SpeechInput");
    state->userInput = std::make_shared<FrameInjector>("UserTextInput");

    state->transportOutput = std::make_shared<pipeline::TransportOutputProcessor>();
    state->transportOutput->addTransport(options.transport, options.sendAllFramesToTransport
        ? std::nullopt : std::optional<transport::FrameType>(transport::FrameType::SYNTHESIS));

    //  LLM (optional)
    state->llmTransport = options.llmTransport;
    if (!state->llmTransport && config.has_llm() && !config.llm().endpoint().empty()) {
        llm::LLMConfig llmConfig;
        llmConfig.endpoint     = config.llm().endpoint();
        llmConfig.apiKey       = config.llm().api_key();
        llmConfig.model        = config.llm().model();
        llmConfig.systemPrompt = options.systemPrompt;
        llmConfig.temperature  = 0.1f;
        llmConfig.maxTokens    = 8000;
        llmConfig.nCtx         = 64000;
        llmConfig.timeoutMs    = 30000;
        llmConfig.streaming    = true;
        state->llmTransport = std::make_shared<llm::OpenAITransport>(llmConfig);
        auto systemTemplate = llm::PromptTemplate::create(state->llmTransport->getPromptChatFormat());
        systemTemplate->addSystem(llmConfig.systemPrompt + "\n\n{tool_descriptions}");
        state->llmTransport->setPromptTemplate(std::move(systemTemplate));
        state->llmTransport->registerTemplateVar("tool_descriptions",
            [tools = state->tools](llm::ChatFormat format) { return tools->renderToolDescriptions(format); });
        spdlog::info("[VAEngine] LLM: {} ({})", llmConfig.endpoint, llmConfig.model);
    }
    if (state->llmTransport) {
        if (!options.responseFormatInstruction.empty())
            state->llmTransport->setResponseFormatInstruction(options.responseFormatInstruction);
        state->llmTransport->onError = [](int code, const std::string& message) {
            spdlog::error("[VAEngine] LLM error [{}]: {}", code, message);
        };
        state->llm = std::make_shared<llm::LLMProcessor>(state->llmTransport);
        state->llm->setHistoryEnabled(true);
        state->llm->setMaxHistoryLength(10);
        state->llm->setProcessPartialTranscriptions(false);
        state->llm->setConfidenceThreshold(0.07f);
        state->llm->setFirePolicy(std::make_shared<llm::FinalOnlyFirePolicy>());
        state->llm->setStreamSpeech(true);  // speak each sentence as the LLM streams it
        state->llm->setToolDispatcher(state->tools);
    } else {
        spdlog::info("[VAEngine] No LLM configured: the assistant only speaks say() text");
    }

    state->options = std::move(options);
    return std::unique_ptr<VAEngine>(new VAEngine(std::move(state)));
}

VAEngine::VAEngine(std::unique_ptr<State> state) : _state(std::move(state)) {}

VAEngine::~VAEngine() { stop(); }

bool VAEngine::addProcessor(Slot slot, std::shared_ptr<FrameProcessor> processor) {
    if (_state->pipeline || !processor) return false;
    _state->slots[slot].push_back(std::move(processor));
    return true;
}

void VAEngine::registerTool(const llm::Tool& tool) { _state->tools->registerTool(tool); }

bool VAEngine::start(std::string* error) {
    auto fail = [error](const std::string& message) {
        spdlog::error("[VAEngine] {}", message);
        if (error) *error = message;
        return false;
    };
    if (_state->pipeline) return fail("already started");
    auto& transport = *_state->options.transport;
    if (!transport.initialize() || !transport.start()) return fail("transport " + transport.getName() + " failed to start");

    auto pipeline = std::make_shared<pipeline::Pipeline>();
    for (auto& processor : _state->chain()) pipeline->addProcessor(processor);
    if (auto& reporter = _state->options.metricsReporter) {
        metrics::MetricsBinder::bind(*pipeline, _state->options.metricsConfig, reporter);
        reporter->addTransport(_state->options.transport);
    }
    if (!pipeline->build()) return fail("pipeline failed to build");
    if (!pipeline->start()) return fail("pipeline failed to start");
    _state->pipeline = pipeline;
    spdlog::info("[VAEngine] Started: {}", describe());
    return true;
}

void VAEngine::stop() {
    if (!_state->pipeline) return;
    _state->pipeline->stop();
    _state->options.transport->stop();
    _state->pipeline.reset();
}

void VAEngine::say(const std::string& text) {
    auto frame = std::make_shared<transport::TextFrame>();
    frame->text = text;
    frame->isFinal = true;
    _state->speech->inject(frame);
}

void VAEngine::injectUserText(const std::string& text) {
    if (!_state->llm) {
        spdlog::warn("[VAEngine] injectUserText ignored: no LLM configured");
        return;
    }
    // The same frames a spoken turn ends with: the final transcript, then the user's turn end.
    auto transcription = std::make_shared<transport::TranscriptionFrame>();
    transcription->isFinal = true;
    transcription->transcriptionResult.text = text;
    transcription->transcriptionResult.confidence = 1.0f;
    _state->userInput->inject(transcription);
    _state->userInput->inject(std::make_shared<transport::TurnStateFrame>(
        transport::TurnStateFrame::TurnActor::USER, transport::TurnStateFrame::TurnState::END,
        transport::TurnStateFrame::TurnType::NORMAL));
}

void VAEngine::interrupt() { _state->trigger->trigger(transport::TriggerType::BUTTON_PRESS); }

bool VAEngine::hasLLM() const { return _state->llm != nullptr; }

std::string VAEngine::describe() const {
    std::string description;
    for (const auto& processor : _state->chain()) {
        if (!description.empty()) description += " → ";
        description += processor->getName();
        const std::string provider = processor->getProviderName();
        if (!provider.empty()) description += " (" + provider + ")";
    }
    return description;
}

std::shared_ptr<AudioRingBuffer> VAEngine::inputRingBuffer() const { return _state->ringBuffer; }
std::shared_ptr<llm::LLMTransport> VAEngine::llmTransport() const { return _state->llmTransport; }
std::shared_ptr<pipeline::Pipeline> VAEngine::pipeline() const { return _state->pipeline; }

} // namespace va
