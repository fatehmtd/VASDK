#include "engine/Session.h"

#include "AudioRingBuffer.h"
#include "Pipeline.h"
#include "TTSFrameProcessor.h"
#include "TranscriptionProcessor.h"
#include "TransportInputProcessor.h"
#include "TransportOutputProcessor.h"
#include "TriggerProcessor.h"
#include "TurnAssemblerProcessor.h"
#include "TurnDetectionProcessor.h"
#include "VADFrameProcessor.h"
#include "WakeWordProcessor.h"
#include "engine/EventTapProcessor.h"
#include "engine/Runtime.h"
#include "context/ContextProvider.h"
#include "flow/DialogProcessor.h"
#include "llm/LLMFirePolicy.h"
#include "llm/LLMProcessor.h"
#include "llm/OpenAITransport.h"
#include "llm/PromptTemplate.h"
#include "llm/ToolDispatchProcessor.h"
#include "vasdk/engine/events.pb.h"
#include "metrics/MetricsBinder.h"
#include "metrics/MetricsReporter.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace va {
namespace engine {

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


// The config's turn_taking.backchannel values replace the app's, one by one.
void applyBackchannel(const vasdk::engine::Backchannel& config, pipeline::BackchannelConfig& backchannel) {
    using Config = vasdk::engine::Backchannel;
    if (config.has_enabled()) backchannel.enabled = config.enabled();
    if (config.has_pending_playback())
        backchannel.pendingPlaybackMode = config.pending_playback() == Config::PAUSE ? transport::PendingPlaybackMode::Pause
            : config.pending_playback() == Config::CONTINUE ? transport::PendingPlaybackMode::Continue
            : transport::PendingPlaybackMode::Duck;
    if (config.has_history())
        backchannel.historyMode = config.history() == Config::APPEND_TO_HISTORY
            ? pipeline::BackchannelHistoryMode::AppendToHistory : pipeline::BackchannelHistoryMode::Drop;
    if (config.has_tail_overlap())
        backchannel.tailOverlapMode = config.tail_overlap() == Config::CLASSIFY
            ? pipeline::TailOverlapMode::Classify : pipeline::TailOverlapMode::TreatAsTurn;
    if (config.has_duck_gain_db()) backchannel.duckGainDecibels = config.duck_gain_db();
    if (config.has_duck_ms()) backchannel.duckDurationMs = static_cast<uint32_t>(std::max(0, config.duck_ms()));
    if (config.has_commit_after_ms()) backchannel.commitAfterPendingMs = static_cast<uint32_t>(std::max(0, config.commit_after_ms()));
    if (config.has_final_transcription_wait_ms())
        backchannel.finalTranscriptionWaitMs = static_cast<uint32_t>(std::max(0, config.final_transcription_wait_ms()));
    if (config.has_tail_overlap_ms()) backchannel.tailOverlapMs = static_cast<uint32_t>(std::max(0, config.tail_overlap_ms()));
    if (config.has_max_backchannel_words())
        backchannel.maxBackchannelWords = static_cast<uint32_t>(std::max(0, config.max_backchannel_words()));
    if (config.has_min_interrupt_confidence()) backchannel.minInterruptConfidence = config.min_interrupt_confidence();
    if (config.has_low_confidence_max_words())
        backchannel.lowConfidenceMaxWords = static_cast<uint32_t>(std::max(0, config.low_confidence_max_words()));
    if (config.phrases_size() > 0) backchannel.backchannelPhrases.assign(config.phrases().begin(), config.phrases().end());
    if (config.interrupt_words_size() > 0)
        backchannel.interruptWords.assign(config.interrupt_words().begin(), config.interrupt_words().end());
}

// The preset's VAD timings, then any turn_taking.voice_activity value set in the config.
pipeline::VADConfig voiceActivityConfig(const vasdk::engine::TurnTaking& turnTaking) {
    pipeline::VADConfig vadConfig;
    switch (turnTaking.preset()) {
    case vasdk::engine::TurnTaking::SNAPPY:
        vadConfig.vadThresholdMs = 150, vadConfig.minSpeechDurationMs = 150;
        vadConfig.maxSpeechPauseDurationMs = 200, vadConfig.maxSpeechEndDurationMs = 150;
        break;
    case vasdk::engine::TurnTaking::PATIENT:
        vadConfig.vadThresholdMs = 250, vadConfig.minSpeechDurationMs = 400;
        vadConfig.maxSpeechPauseDurationMs = 500, vadConfig.maxSpeechEndDurationMs = 600;
        break;
    default:
        vadConfig.vadThresholdMs = 250, vadConfig.minSpeechDurationMs = 250;
        vadConfig.maxSpeechPauseDurationMs = 500, vadConfig.maxSpeechEndDurationMs = 250;
        break;
    }
    // Echo cancellation turns the user's voice down while the assistant talks: confirm sooner then.
    vadConfig.bargeInMinSpeechDurationMs = 150;
    const auto& voice = turnTaking.voice_activity();
    auto milliseconds = [](int32_t value) { return static_cast<uint64_t>(std::max(0, value)); };
    if (voice.has_start_ms()) vadConfig.vadThresholdMs = milliseconds(voice.start_ms());
    if (voice.has_min_speech_ms()) vadConfig.minSpeechDurationMs = milliseconds(voice.min_speech_ms());
    if (voice.has_pause_ms()) vadConfig.maxSpeechPauseDurationMs = milliseconds(voice.pause_ms());
    if (voice.has_end_silence_ms()) vadConfig.maxSpeechEndDurationMs = milliseconds(voice.end_silence_ms());
    if (voice.has_resume_silent_frames()) vadConfig.resumingSilenceFramesToPause = std::max(1, voice.resume_silent_frames());
    if (voice.has_barge_in_min_speech_ms()) vadConfig.bargeInMinSpeechDurationMs = milliseconds(voice.barge_in_min_speech_ms());
    return vadConfig;
}

} // namespace

struct Session::State {
    // First member, so it is destroyed last: the providers go back once no processor uses them.
    struct Lease {
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<Runtime::ProviderSet> providers;
        ~Lease() { if (runtime && providers) runtime->release(std::move(providers)); }
    } lease;
    Options options;
    std::string logTag;   // "[VAEngine]" or "[Session <id>]"
    std::shared_ptr<EventDispatcher> events = std::make_shared<EventDispatcher>();
    std::shared_ptr<AudioRingBuffer> ringBuffer;

    std::shared_ptr<pipeline::TransportInputProcessor> transportInput;
    std::shared_ptr<pipeline::TriggerProcessor> trigger;
    std::shared_ptr<pipeline::WakeWordProcessor> wakeWord;   // WAKE_WORD and HYBRID modes
    std::shared_ptr<pipeline::VADFrameProcessor> vad;
    std::shared_ptr<pipeline::TurnDetectionProcessor> turnDetection;
    std::shared_ptr<pipeline::TranscriptionFrameProcessor> transcription;
    std::shared_ptr<pipeline::AgentStateProcessor> agentState;
    std::shared_ptr<FrameProcessor> bargeInHandler;   // only with back-channel handling off
    std::shared_ptr<pipeline::TurnAssemblerProcessor> turnAssembler;
    std::shared_ptr<FrameInjector> userInput;
    std::shared_ptr<flow::DialogProcessor> dialog;   // only with Options::agent
    std::shared_ptr<llm::LLMTransport> llmTransport;
    std::shared_ptr<llm::LLMProcessor> llm;
    std::shared_ptr<llm::ToolDispatchProcessor> tools = std::make_shared<llm::ToolDispatchProcessor>();
    std::shared_ptr<FrameInjector> speech;
    std::shared_ptr<EventTapProcessor> eventTap;
    std::shared_ptr<pipeline::TTSFrameProcessor> tts;
    std::shared_ptr<pipeline::TransportOutputProcessor> transportOutput;

    std::map<Slot, std::vector<std::shared_ptr<FrameProcessor>>> slots;
    std::shared_ptr<pipeline::Pipeline> pipeline;

    std::vector<std::shared_ptr<FrameProcessor>> chain() const {
        std::vector<std::shared_ptr<FrameProcessor>> processors{transportInput, trigger};
        if (wakeWord) processors.push_back(wakeWord);
        auto addSlot = [&](Slot slot) {
            auto found = slots.find(slot);
            if (found != slots.end()) processors.insert(processors.end(), found->second.begin(), found->second.end());
        };
        addSlot(Slot::AfterInput);
        processors.insert(processors.end(), {vad, turnDetection, transcription});
        addSlot(Slot::AfterTranscription);
        processors.push_back(agentState);
        if (bargeInHandler) processors.push_back(bargeInHandler);
        if (turnAssembler) processors.insert(processors.end(), {turnAssembler, userInput});
        if (dialog) processors.push_back(dialog);
        if (llm) processors.insert(processors.end(), {llm, tools});
        addSlot(Slot::BeforeSynthesis);
        processors.insert(processors.end(), {speech, eventTap, tts});
        addSlot(Slot::BeforeOutput);
        processors.push_back(transportOutput);
        return processors;
    }
};

Session::Session(std::shared_ptr<Runtime> runtime, Options options, std::string* error)
    : _state(createState(std::move(runtime), std::move(options), error)) {}

Session::~Session() {
    if (_state) stop();
}

std::unique_ptr<Session::State> Session::createState(std::shared_ptr<Runtime> runtime, Options options,
                                                     std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<State> {
        if (error) *error = message;
        return nullptr;
    };
    if (!options.transport) return fail("Options::transport is required");
    auto state = std::make_unique<State>();
    state->lease.providers = runtime->acquire(error);
    if (!state->lease.providers) return nullptr;
    state->lease.runtime = runtime;
    const auto& providers = *state->lease.providers;
    const auto& config = runtime->config();
    const auto& audioOptions = runtime->options();
    state->logTag = options.id.empty() ? "[VAEngine]" : "[Session " + options.id + "]";
    state->events->setSessionId(options.id);
    state->ringBuffer = providers.ringBuffer;

    auto& transport = *options.transport;
    transport.setInputAudioConfig(audioOptions.inputAudio);
    transport.setOutputAudioConfig(audioOptions.outputAudio);
    transport.setFrameSize(audioOptions.frameSize);
    transport.setRingBuffer(state->ringBuffer);
    if (options.metricsReporter) transport.setMetricsConfig(options.metricsConfig.transport);

    //  Processors
    state->transportInput = std::make_shared<pipeline::TransportInputProcessor>();
    state->transportInput->addTransport(options.transport);
    state->trigger = std::make_shared<pipeline::TriggerProcessor>();

    const auto& turnTaking = config.turn_taking();
    applyBackchannel(turnTaking.backchannel(), options.backchannel);
    const bool balanced = turnTaking.preset() == vasdk::engine::TurnTaking::BALANCED;
    const float lookbackFactor = turnTaking.voice_activity().has_lookback_factor()
        ? std::max(0.0f, turnTaking.voice_activity().lookback_factor()) : (balanced ? 3.0f : 5.0f);
    state->vad = std::make_shared<pipeline::VADFrameProcessor>(providers.vad, lookbackFactor);
    state->vad->setMinFrameSize(audioOptions.frameSize);
    state->vad->setConfig(voiceActivityConfig(turnTaking));
    state->vad->setRingBuffer(state->ringBuffer);

    state->turnDetection = std::make_shared<pipeline::TurnDetectionProcessor>(providers.turn);

    state->transcription = std::make_shared<pipeline::TranscriptionFrameProcessor>(providers.stt);
    state->transcription->configSilenceBehavior(false, false, 1000);
    state->transcription->setStreamingMode(turnTaking.streaming_transcription());
    state->transcription->setRingBuffer(state->ringBuffer);

    state->agentState = std::make_shared<pipeline::AgentStateProcessor>();
    const auto& activation = config.activation();
    state->agentState->setAlwaysOn(activation.mode() == vasdk::engine::Activation::ALWAYS_ON);
    state->agentState->setFollowUpListenMs(
        activation.follow_up().has_listen_ms() ? activation.follow_up().listen_ms() : 8000);
    if (providers.wakeWord) {
        state->wakeWord = std::make_shared<pipeline::WakeWordProcessor>(providers.wakeWord);
        state->wakeWord->setRingBuffer(state->ringBuffer);
        state->wakeWord->setAgentIdle([agentState = std::weak_ptr<pipeline::AgentStateProcessor>(state->agentState)] {
            auto current = agentState.lock();
            return current && current->getState() == transport::AgentState::IDLE;
        });
    }
    state->agentState->setBackchannelConfig(options.backchannel);
    if (turnTaking.has_interrupt_timeout_ms())
        state->agentState->setInterruptTimeoutMs(static_cast<uint32_t>(std::max(0, turnTaking.interrupt_timeout_ms())));
    state->vad->setAgentSpeaking([agentState = std::weak_ptr<pipeline::AgentStateProcessor>(state->agentState)] {
        auto current = agentState.lock();
        return current && current->getState() == transport::AgentState::SPEAKING;
    });
    // Batch transcription only, as the desktop app always had it; the call center (streaming)
    // never cleared the STT buffer on a barge-in.
    if (!options.backchannel.enabled && !turnTaking.streaming_transcription())
        state->bargeInHandler = std::make_shared<BargeInHandler>(providers.stt);

    state->tts = std::make_shared<pipeline::TTSFrameProcessor>(providers.tts);
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
    state->agentState->setProtectedPlaybackMsProvider(
        [tts = std::weak_ptr<pipeline::TTSFrameProcessor>(state->tts)] {
            auto ttsProcessor = tts.lock();
            return ttsProcessor ? ttsProcessor->getProtectedPlaybackMs() : 0;
        });
    state->speech = std::make_shared<FrameInjector>("SpeechInput");
    state->eventTap = std::make_shared<EventTapProcessor>(state->events);
    // Tool calls don't reach the tap: results go back upstream to the LLM.
    state->tools->setResultObserver([events = std::weak_ptr<EventDispatcher>(state->events)](
                                        const std::string& name, const std::string& argsJson,
                                        const std::string& resultJson) {
        auto dispatcher = events.lock();
        if (!dispatcher) return;
        vasdk::engine::Event event;
        auto* call = event.mutable_tool_call();
        call->set_name(name);
        call->set_arguments_json(argsJson);
        call->set_result_json(resultJson);
        dispatcher->post(std::move(event));
    });
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
        spdlog::info("{} LLM: {} ({})", state->logTag, llmConfig.endpoint, llmConfig.model);
    }
    if (state->llmTransport) {
        if (!options.responseFormatInstruction.empty())
            state->llmTransport->setResponseFormatInstruction(options.responseFormatInstruction);
        state->llmTransport->onError = [tag = state->logTag](int code, const std::string& message) {
            spdlog::error("{} LLM error [{}]: {}", tag, code, message);
        };
        state->llm = std::make_shared<llm::LLMProcessor>(state->llmTransport);
        state->llm->setHistoryEnabled(true);
        const auto& llmSettings = config.llm();
        state->llm->setMaxHistoryLength(llmSettings.history() > 0 ? llmSettings.history() : 10);
        if (llmSettings.speculative_firing())
            state->llm->setSpeculativeFiring(true, llmSettings.speculative_min_words() > 0
                                                       ? llmSettings.speculative_min_words() : 3);
        state->llm->setToolFillerPhrases({llmSettings.tool_filler_phrases().begin(),
                                          llmSettings.tool_filler_phrases().end()});
        state->llm->setProcessPartialTranscriptions(false);
        state->llm->setConfidenceThreshold(0.07f);
        state->llm->setStreamSpeech(true);  // speak each sentence as the LLM streams it
        state->llm->setToolDispatcher(state->tools);
    } else if (!options.agent) {
        spdlog::info("{} No LLM or agent configured: the assistant only speaks say() text", state->logTag);
    }

    //  Agent (optional): answers turns from its pages, before the LLM
    if (options.agent) {
        state->dialog = std::make_shared<flow::DialogProcessor>(options.agent);
        state->dialog->setPassUnmatchedTurns(state->llm != nullptr);
        state->dialog->setToolCaller(flow::ToolRunner::caller(std::make_shared<flow::ToolRunner>(options.agent, config.security())));
        if (options.context) {
            state->dialog->setContextSource([context = options.context] {
                google::protobuf::Struct fields;
                const auto json = context->getContext();
                if (json.is_object()) {
                    const auto status = google::protobuf::util::JsonStringToMessage(json.dump(), &fields);
                    if (!status.ok()) spdlog::warn("[Session] $context unreadable: {}", status.message());
                }
                return fields;
            });
        }
        state->dialog->setReplyObserver([events = std::weak_ptr<EventDispatcher>(state->events)](
                                            const flow::Reply& reply, const flow::FlowRuntime& conversation) {
            auto dispatcher = events.lock();
            if (!dispatcher) return;
            for (const auto& call : reply.toolCalls) {
                vasdk::engine::Event event;
                auto* tool = event.mutable_tool_call();
                tool->set_name(call.name);
                tool->set_arguments_json(flow::redact(call.argumentsJson, reply.sensitiveValues));
                tool->set_result_json(flow::redact(call.resultJson, reply.sensitiveValues));
                dispatcher->post(std::move(event));
            }
            for (const auto& change : reply.pageChanges) {
                vasdk::engine::Event event;
                auto* changed = event.mutable_page_changed();
                changed->set_from_flow(change.fromFlow);
                changed->set_from_page(change.fromPage);
                changed->set_to_flow(change.toFlow);
                changed->set_to_page(change.toPage);
                changed->set_reason(change.reason);
                dispatcher->post(std::move(event));
            }
            if (reply.endsSession) {
                vasdk::engine::Event event;
                event.mutable_session_ended()->set_flow(conversation.flowName());
                event.mutable_session_ended()->set_page(conversation.pageName());
                dispatcher->post(std::move(event));
            }
        });
    }
    if (state->dialog) {
        // The greeting waits for someone to hear it: a browser peer, a phone call. Handlers the
        // app set on the transport still run.
        auto& audio = *options.transport;
        auto dialog = std::weak_ptr<flow::DialogProcessor>(state->dialog);
        audio.onConnected = [previous = audio.onConnected, dialog] {
            if (previous) previous();
            if (auto processor = dialog.lock()) processor->setConnected(true);
        };
        audio.onDisconnected = [previous = audio.onDisconnected, dialog] {
            if (previous) previous();
            if (auto processor = dialog.lock()) processor->setConnected(false);
        };
    }
    if (state->llm || state->dialog) {
        state->turnAssembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
        state->turnAssembler->setFirePolicy(std::make_shared<llm::FinalOnlyFirePolicy>());
    }

    state->options = std::move(options);
    return state;
}

bool Session::addProcessor(Slot slot, std::shared_ptr<FrameProcessor> processor) {
    if (_state->pipeline || !processor) return false;
    _state->slots[slot].push_back(std::move(processor));
    return true;
}

void Session::registerTool(const llm::Tool& tool) { _state->tools->registerTool(tool); }

int Session::addListener(engine::EventListener listener) { return _state->events->addListener(std::move(listener)); }
void Session::removeListener(int id) { _state->events->removeListener(id); }

bool Session::start(std::string* error) {
    auto fail = [this, error](const std::string& message) {
        spdlog::error("{} {}", _state->logTag, message);
        if (error) *error = message;
        return false;
    };
    if (_state->pipeline) return fail("already started");
    auto& transport = *_state->options.transport;
    if (!transport.initialize() || !transport.start()) return fail("transport " + transport.getName() + " failed to start");
    if (_state->dialog) _state->dialog->setConnected(transport.isConnected());

    auto pipeline = std::make_shared<pipeline::Pipeline>();
    for (auto& processor : _state->chain()) pipeline->addProcessor(processor);
    if (auto& reporter = _state->options.metricsReporter) {
        metrics::MetricsBinder::bind(*pipeline, _state->options.metricsConfig, reporter);
        reporter->addTransport(_state->options.transport);
    }
    if (!pipeline->build() || !pipeline->start()) {
        pipeline->stop();
        transport.stop();
        return fail("pipeline failed to start");
    }
    _state->pipeline = pipeline;
    spdlog::info("{} Started: {}", _state->logTag, describe());
    return true;
}

void Session::stop() {
    if (!_state->pipeline) return;
    _state->pipeline->stop();
    _state->options.transport->stop();
    _state->pipeline.reset();
}

void Session::say(const std::string& text) {
    auto frame = std::make_shared<transport::TextFrame>();
    frame->text = text;
    frame->isFinal = true;
    _state->speech->inject(frame);
}

void Session::injectUserText(const std::string& text) {
    if (!_state->llm && !_state->dialog) {
        spdlog::warn("{} injectUserText ignored: no LLM or agent configured", _state->logTag);
        return;
    }
    // What a spoken turn reaches the LLM as: the final transcript (for listeners), then the turn.
    auto transcription = std::make_shared<transport::TranscriptionFrame>();
    transcription->isFinal = true;
    transcription->transcriptionResult.text = text;
    transcription->transcriptionResult.confidence = 1.0f;
    _state->userInput->inject(transcription);
    auto turn = std::make_shared<transport::UserTurnFrame>();
    turn->text = text;
    turn->confidence = 1.0f;
    _state->userInput->inject(turn);
}

void Session::selectChoice(int index) {
    if (!_state->dialog) {
        spdlog::warn("{} selectChoice ignored: no agent", _state->logTag);
        return;
    }
    _state->dialog->selectChoice(index);
}

void Session::interrupt() { _state->trigger->trigger(transport::TriggerType::BUTTON_PRESS); }

void Session::pushToTalk(bool held) {
    _state->trigger->trigger(transport::TriggerType::PUSH_TO_TALK, held ? "down" : "up");
}

void Session::setFollowUpListenMs(int ms) { _state->agentState->setFollowUpListenMs(ms); }

void Session::setSessionParam(const std::string& name, const google::protobuf::Value& value) {
    if (!_state->dialog) {
        spdlog::warn("{} setSessionParam ignored: no agent", _state->logTag);
        return;
    }
    _state->dialog->setSessionParam(name, value);
}

void Session::setEntityValues(const std::string& name, const std::vector<vasdk::nlu::EntityValue>& values) {
    if (!_state->dialog) {
        spdlog::warn("{} setEntityValues ignored: no agent", _state->logTag);
        return;
    }
    _state->dialog->setEntityValues(name, values);
}

const std::string& Session::id() const { return _state->options.id; }
bool Session::hasLLM() const { return _state->llm != nullptr; }

std::string Session::describe() const {
    std::string description;
    for (const auto& processor : _state->chain()) {
        if (!description.empty()) description += " → ";
        description += processor->getName();
        const std::string provider = processor->getProviderName();
        if (!provider.empty()) description += " (" + provider + ")";
    }
    return description;
}

std::shared_ptr<AudioRingBuffer> Session::inputRingBuffer() const { return _state->ringBuffer; }
std::shared_ptr<llm::ToolDispatchProcessor> Session::toolDispatcher() const { return _state->tools; }
std::shared_ptr<llm::LLMTransport> Session::llmTransport() const { return _state->llmTransport; }
std::shared_ptr<pipeline::Pipeline> Session::pipeline() const { return _state->pipeline; }

} // namespace engine
} // namespace va
