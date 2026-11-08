#include "engine/Session.h"

#include "AudioDSPProcessor.h"
#include "AudioRingBuffer.h"
#include "Pipeline.h"
#include "SpeakerIdentificationProcessor.h"
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
#include "engine/RemoteControl.h"
#include "engine/Runtime.h"
#include "context/ContextProvider.h"
#include "flow/DialogProcessor.h"
#include "nlu/EmbeddingNluProvider.h"
#include "nlu/LlmEntityExtractor.h"
#include "nlu/LlmNluProvider.h"
#include "nlu/TaggerEntityExtractor.h"
#include "llm/LLMFirePolicy.h"
#include "llm/Completion.h"
#include "llm/LLMProcessor.h"
#include "llm/AnthropicTransport.h"
#include "llm/OpenAITransport.h"
#include "llm/PromptTemplate.h"
#include "llm/ToolDispatchProcessor.h"
#include "vasdk/engine/events.pb.h"
#include "metrics/MetricsBinder.h"
#include "metrics/MetricsReporter.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <map>
#include <optional>
#include <vector>

namespace va {
namespace engine {

namespace {

using pipeline::FlowDirection;
using pipeline::FrameProcessor;

// An endpoint reached without the network: localhost, 127.x, ::1 (LM Studio, a llama.cpp server).
bool onThisMachine(const std::string& endpoint) {
    const auto start = endpoint.find("://");
    const auto host = endpoint.substr(start == std::string::npos ? 0 : start + 3);
    for (const char* local : {"localhost", "127.", "[::1]"})
        if (host.rfind(local, 0) == 0) return true;
    return false;
}

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
    explicit BargeInHandler(std::shared_ptr<pipeline::TranscriptionFrameProcessor> transcription)
        : _transcription(std::move(transcription)) {}
    std::string getName() const override { return "BargeInHandler"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override {
        return frame->getType() == transport::FrameType::AGENT_STATE;
    }
    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override {
        if (direction == FlowDirection::DOWNSTREAM) {
            auto agentFrame = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame);
            if (agentFrame && agentFrame->transition == transport::AgentStateTransition::BARGE_IN_INTERRUPT) {
                if (auto provider = _transcription->provider()) provider->clearAudioBuffer();
                spdlog::info("[BargeInHandler] Cleared transcription buffer");
            }
        }
        pushDownstream(frame);
    }

private:
    std::shared_ptr<pipeline::TranscriptionFrameProcessor> _transcription;   // its provider changes when the network drops
};


// An llm_profiles entry as an LLMConfig: `base` (llm's) with what the profile sets. A profile of
// another API than llm's starts from that API's defaults, not from llm's endpoint and model.
llm::LLMConfig profileConfig(const vasdk::engine::LlmConfig& llm, const vasdk::engine::LlmProfile& profile, llm::LLMConfig base) {
    const auto api = profile.has_api() ? profile.api() : llm.api();
    if (api != llm.api()) {
        const bool anthropic = api == vasdk::engine::LlmConfig::ANTHROPIC;
        base.endpoint = anthropic ? "https://api.anthropic.com/v1/messages" : "";
        base.model = anthropic ? "claude-opus-5-5" : "";
        base.apiKey.clear();
        base.reasoningEffort.clear();
    }
    if (!profile.endpoint().empty()) base.endpoint = profile.endpoint();
    if (!profile.model().empty()) base.model = profile.model();
    if (!profile.api_key().empty()) base.apiKey = profile.api_key();
    if (!profile.effort().empty()) base.reasoningEffort = profile.effort();
    return base;
}

bool profileIsAnthropic(const vasdk::engine::LlmConfig& llm, const vasdk::engine::LlmProfile& profile) {
    return (profile.has_api() ? profile.api() : llm.api()) == vasdk::engine::LlmConfig::ANTHROPIC;
}

// The agent's LLM, off while the network is down, and for a while after a request fails, so
// each turn doesn't wait out the timeout of an LLM that can't be reached.
class GatedCompletion : public llm::Completion {
public:
    GatedCompletion(std::shared_ptr<const llm::Completion> inner, std::shared_ptr<std::atomic<bool>> offline)
        : _inner(std::move(inner)), _offline(std::move(offline)) {}
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override {
        return completeWithin(messages, 0);
    }
    // A question given up at its time limit doesn't rest the LLM: it's slow, not unreachable.
    std::optional<std::string> completeWithin(const std::vector<llm::ConversationMessage>& messages, int timeLimitMs) const override {
        using namespace std::chrono;
        const auto clock = [] { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); };
        const auto now = clock();
        if (*_offline || now - _failedAt.load() < kRetryAfterMs) return std::nullopt;
        auto reply = timeLimitMs > 0 ? _inner->completeWithin(messages, timeLimitMs) : _inner->complete(messages);
        if (!reply && (timeLimitMs <= 0 || clock() - now < timeLimitMs)) _failedAt = now;
        return reply;
    }

private:
    static constexpr long long kRetryAfterMs = 30000;
    std::shared_ptr<const llm::Completion> _inner;
    std::shared_ptr<std::atomic<bool>> _offline;
    mutable std::atomic<long long> _failedAt{-kRetryAfterMs};
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
    if (config.has_reject_echo()) backchannel.rejectEcho = config.reject_echo();
    if (config.has_echo_min_share()) backchannel.echoMinShare = config.echo_min_share();
    if (config.has_echo_words_wait_ms()) backchannel.echoWordsWaitMs = static_cast<uint32_t>(std::max(0, config.echo_words_wait_ms()));
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
    std::shared_ptr<pipeline::AudioDSPProcessor> audioDsp;   // only with providers.audio_dsp
    std::shared_ptr<pipeline::TriggerProcessor> trigger;
    std::shared_ptr<pipeline::WakeWordProcessor> wakeWord;   // WAKE_WORD and HYBRID modes
    std::shared_ptr<pipeline::VADFrameProcessor> vad;
    std::shared_ptr<pipeline::TurnDetectionProcessor> turnDetection;
    std::shared_ptr<pipeline::TranscriptionFrameProcessor> transcription;
    std::shared_ptr<pipeline::SpeakerIdentificationProcessor> speakerIdentification;   // only with providers.speaker_embedding
    std::shared_ptr<pipeline::AgentStateProcessor> agentState;
    std::shared_ptr<FrameProcessor> bargeInHandler;   // only with back-channel handling off
    std::shared_ptr<pipeline::TurnAssemblerProcessor> turnAssembler;
    std::shared_ptr<FrameInjector> userInput;
    std::shared_ptr<flow::DialogProcessor> dialog;   // only with Options::agent
    std::shared_ptr<llm::LLMTransport> llmTransport;
    bool llmFromProvider = false;   // providers.llm: it reads the system prompt from its config
    std::shared_ptr<llm::LLMProcessor> llm;
    std::shared_ptr<llm::ToolDispatchProcessor> tools = std::make_shared<llm::ToolDispatchProcessor>();
    std::shared_ptr<FrameInjector> speech;
    std::shared_ptr<EventTapProcessor> eventTap;
    std::shared_ptr<pipeline::TTSFrameProcessor> tts;
    std::shared_ptr<pipeline::TransportOutputProcessor> transportOutput;

    std::map<Slot, std::vector<std::shared_ptr<FrameProcessor>>> slots;
    std::shared_ptr<pipeline::Pipeline> pipeline;

    // Network fallback: the local providers while the network is down. Guarded by fallbackMutex.
    std::shared_ptr<std::atomic<bool>> llmOffline = std::make_shared<std::atomic<bool>>(false);
    mutable std::mutex fallbackMutex;
    // The agent's conversation after its last reply (FlowRuntime::snapshot). Guarded by snapshotMutex.
    mutable std::mutex snapshotMutex;
    vasdk::flow::ConversationSnapshot snapshot;
    bool offline = false;
    bool fallbacksLoaded = false;
    void switchProviders(bool toFallbacks, const std::string& reason);
    void networkLost(const std::string& reason, bool dropLlm);
    void networkBack();
    void post(const std::string& kind, const std::string& from, const std::string& to, const std::string& reason) const;
    std::unique_ptr<RemoteControl> remote;   // remote_control: watched and driven over HTTP
    // Last, so it's joined before anything it uses is destroyed.
    struct Loader {
        std::thread thread;
        ~Loader() { if (thread.joinable()) thread.join(); }
    } fallbackLoader;

    std::vector<std::shared_ptr<FrameProcessor>> chain() const {
        std::vector<std::shared_ptr<FrameProcessor>> processors{transportInput};
        if (audioDsp) processors.push_back(audioDsp);
        processors.push_back(trigger);
        if (wakeWord) processors.push_back(wakeWord);
        auto addSlot = [&](Slot slot) {
            auto found = slots.find(slot);
            if (found != slots.end()) processors.insert(processors.end(), found->second.begin(), found->second.end());
        };
        addSlot(Slot::AfterInput);
        processors.insert(processors.end(), {vad, turnDetection, transcription});
        // After STT: embedding the whole utterance takes ~70 ms per second of speech
        if (speakerIdentification) processors.push_back(speakerIdentification);
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

void Session::State::post(const std::string& kind, const std::string& from, const std::string& to,
                          const std::string& reason) const {
    vasdk::engine::Event event;
    auto* switched = event.mutable_provider_switched();
    switched->set_kind(kind);
    switched->set_from(from);
    switched->set_to(to);
    switched->set_reason(reason);
    events->post(std::move(event));
    spdlog::warn("{} {}: {} → {} ({})", logTag, kind, from, to, reason);
}

// Under fallbackMutex, once the fallbacks are loaded.
void Session::State::switchProviders(bool toFallbacks, const std::string& reason) {
    const auto& set = *lease.providers;
    if (set.sttFallback) {
        transcription->replaceProvider(toFallbacks ? set.sttFallback : set.stt);
        post("stt", toFallbacks ? set.sttId : set.sttFallbackId, toFallbacks ? set.sttFallbackId : set.sttId, reason);
    }
    if (set.ttsFallback) {
        tts->replaceProvider(toFallbacks ? set.ttsFallback : set.tts);
        post("tts", toFallbacks ? set.ttsId : set.ttsFallbackId, toFallbacks ? set.ttsFallbackId : set.ttsId, reason);
    }
}

void Session::State::networkLost(const std::string& reason, bool dropLlm) {
    std::lock_guard<std::mutex> lock(fallbackMutex);
    if (dropLlm && !llmOffline->exchange(true)) {
        if (dialog) dialog->setPassUnmatchedTurns(false);
        post("llm", "on", "off", reason);
    }
    if (offline) return;
    offline = true;
    if (fallbacksLoaded) {
        switchProviders(true, reason);
        return;
    }
    if (fallbackLoader.thread.joinable()) return;   // loading: it switches when done
    fallbackLoader.thread = std::thread([this, reason] {
        std::string error;
        const bool loaded = lease.runtime->loadFallbacks(*lease.providers, &error);
        if (!loaded) spdlog::warn("{} no fallback provider to switch to: {}", logTag, error);
        std::lock_guard<std::mutex> lock(fallbackMutex);
        fallbacksLoaded = true;
        if (offline) switchProviders(true, reason);
    });
}

void Session::State::networkBack() {
    std::lock_guard<std::mutex> lock(fallbackMutex);
    if (llmOffline->exchange(false)) {
        if (dialog) dialog->setPassUnmatchedTurns(llm != nullptr);
        post("llm", "off", "on", "network up");
    }
    if (!offline) return;
    offline = false;
    if (fallbacksLoaded) switchProviders(false, "network up");
}

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
    state->ringBuffer = providers.processedRingBuffer;   // what the stages hear; the transport fills providers.ringBuffer

    auto& transport = *options.transport;
    transport.setInputAudioConfig(audioOptions.inputAudio);
    transport.setOutputAudioConfig(audioOptions.outputAudio);
    transport.setFrameSize(audioOptions.frameSize);
    transport.setRingBuffer(providers.ringBuffer);
    if (options.metricsReporter) transport.setMetricsConfig(options.metricsConfig.transport);

    //  Processors
    state->transportInput = std::make_shared<pipeline::TransportInputProcessor>();
    state->transportInput->addTransport(options.transport);
    state->trigger = std::make_shared<pipeline::TriggerProcessor>();
    if (providers.audioDsp) {
        state->audioDsp = std::make_shared<pipeline::AudioDSPProcessor>(providers.audioDsp);
        state->audioDsp->setRawAudioRingBuffer(providers.ringBuffer);
        state->audioDsp->setCleanAudioRingBuffer(providers.processedRingBuffer);
    }

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
    if (providers.speakerEmbedding) {
        state->speakerIdentification = std::make_shared<pipeline::SpeakerIdentificationProcessor>(providers.speakerEmbedding);
        state->speakerIdentification->setRingBuffer(state->ringBuffer);
        const auto& identification = config.speaker_identification();
        if (identification.has_threshold()) state->speakerIdentification->setThreshold(identification.threshold());
        if (identification.has_min_speech_seconds()) state->speakerIdentification->setMinSpeechSeconds(identification.min_speech_seconds());
    }

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
    // Back-channels and interruptions in the agent's languages, unless the config lists its own.
    if (options.agent) state->agentState->setLanguages(options.agent->languages());
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
        state->bargeInHandler = std::make_shared<BargeInHandler>(state->transcription);

    state->tts = std::make_shared<pipeline::TTSFrameProcessor>(providers.tts);
    if (options.agent) state->tts->setLanguage(options.agent->languages().all().front());   // dates and numbers said in it
    state->tts->setPlaybackFlush([transport = options.transport] { transport->flushOutputAudio(); });
    state->tts->setPlaybackHold([transport = options.transport](transport::PendingPlaybackMode mode, bool held,
                                                               float duckGainDecibels) {
        transport->holdOutputAudio(mode, held, duckGainDecibels);
    });
    state->tts->setPendingPlayback(options.backchannel.pendingPlaybackMode, options.backchannel.duckGainDecibels,
                                   options.backchannel.duckDurationMs);
    // A cloud provider that fails (a lost connection): its fallback takes over between turns.
    state->transcription->setOnProviderError([raw = state.get()](const std::string& error) { raw->networkLost("stt: " + error, false); });
    state->tts->setOnProviderError([raw = state.get()](const std::string& error) { raw->networkLost("tts: " + error, false); });
    state->agentState->setPlaybackRemainingMsProvider(
        [tts = std::weak_ptr<pipeline::TTSFrameProcessor>(state->tts)] {
            auto ttsProcessor = tts.lock();
            return ttsProcessor ? ttsProcessor->getRemainingReplyPlaybackMs() : -1;
        });
    state->agentState->setSpokenTextProvider([tts = std::weak_ptr<pipeline::TTSFrameProcessor>(state->tts)] {
        auto ttsProcessor = tts.lock();
        return ttsProcessor ? ttsProcessor->currentReplyText() : std::string();
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

    //  LLM (optional): the app's transport, else the config's providers.llm, else its llm endpoint
    state->llmTransport = options.llmTransport;
    std::shared_ptr<const llm::Completion> providerCompletion;
    if (!state->llmTransport && config.providers().has_llm()) {
        std::string reason;
        auto provided = runtime->createLlm(&reason);
        if (!provided.transport) return fail(reason);
        state->llmTransport = std::move(provided.transport);
        providerCompletion = std::move(provided.completion);
        state->llmFromProvider = true;
        auto llmConfig = state->llmTransport->getConfig();
        llmConfig.systemPrompt = options.systemPrompt;   // the tools' descriptions join it at start()
        state->llmTransport->setConfig(llmConfig);
        spdlog::info("{} LLM: provider {}", state->logTag, config.providers().llm().id());
    }
    const bool anthropic = config.llm().api() == vasdk::engine::LlmConfig::ANTHROPIC;
    if (!state->llmTransport && config.has_llm() && (!config.llm().endpoint().empty() || anthropic)) {
        llm::LLMConfig llmConfig;
        llmConfig.endpoint     = config.llm().endpoint().empty() ? "https://api.anthropic.com/v1/messages" : config.llm().endpoint();
        llmConfig.apiKey       = config.llm().api_key();
        llmConfig.model        = config.llm().model();
        llmConfig.systemPrompt = options.systemPrompt;
        llmConfig.temperature  = 0.1f;
        llmConfig.maxTokens    = 8000;
        llmConfig.nCtx         = 64000;
        llmConfig.timeoutMs    = 30000;
        llmConfig.streaming    = true;
        llmConfig.reasoningEffort = config.llm().effort();   // "none": a thinking model answers at once
        if (anthropic) {
            if (llmConfig.model.empty()) llmConfig.model = "claude-opus-5-5";
            state->llmTransport = std::make_shared<llm::AnthropicTransport>(llmConfig);
        } else {
            state->llmTransport = std::make_shared<llm::OpenAITransport>(llmConfig);
        }
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
        // {context} in the system prompt: the app's context as it is when each request is sent
        // (the car's speed, where it's going), not as it was when the session started.
        if (options.context)
            state->llmTransport->registerTemplateVar("context", [context = options.context](llm::ChatFormat) {
                return context->getContext().dump();
            });
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
        // The profiles the conversation may switch to, turn by turn: those of its API
        std::map<std::string, llm::LLMConfig> conversationProfiles;
        for (const auto& profile : config.llm_profiles()) {
            if (profile.name().empty()) continue;
            if (profileIsAnthropic(config.llm(), profile) != anthropic) {
                spdlog::info("{} LLM profile \"{}\": for the agent only (another API than llm's)", state->logTag, profile.name());
                continue;
            }
            conversationProfiles[profile.name()] = profileConfig(config.llm(), profile, state->llmTransport->getConfig());
        }
        if (!conversationProfiles.empty()) state->llm->setProfiles(std::move(conversationProfiles));
        std::vector<std::string> failureReplies{llmSettings.failure_replies().begin(), llmSettings.failure_replies().end()};
        if (failureReplies.empty()) failureReplies = {"Sorry, I couldn't get an answer just now. Could you ask me again?"};
        state->llm->setFailureReplies(std::move(failureReplies));
        // The network down and the LLM elsewhere: the user is told, rather than left waiting for
        // a request that fails. An app's own transport (llama.cpp) and a server on this machine answer.
        if (!options.llmTransport && !state->llmFromProvider && !onThisMachine(state->llmTransport->getConfig().endpoint)) {
            std::vector<std::string> replies(llmSettings.offline_replies().begin(), llmSettings.offline_replies().end());
            if (replies.empty())
                replies.push_back("I can't reach the internet right now, so I can't answer that. Please try again in a moment.");
            state->llm->setOfflineReplies([offline = state->llmOffline] { return offline->load(); }, std::move(replies));
        }
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
        // Understanding: the agent's examples, then paraphrases by meaning, then an LLM.
        auto completion = options.llmCompletion ? options.llmCompletion : providerCompletion;
        llm::LLMConfig completionConfig;
        completionConfig.endpoint = config.llm().endpoint();
        completionConfig.apiKey = config.llm().api_key();
        completionConfig.model = config.llm().model();
        completionConfig.temperature = 0.0f;
        completionConfig.maxTokens = 400;
        completionConfig.timeoutMs = 8000;   // a voice turn can't wait longer
        completionConfig.reasoningEffort = config.llm().effort();
        auto completionOf = [](const llm::LLMConfig& given, bool claude) -> std::shared_ptr<const llm::Completion> {
            if (claude) return std::make_shared<llm::AnthropicCompletion>(given);
            return std::make_shared<llm::OpenAICompletion>(given);
        };
        if (!completion && config.has_llm() && (!config.llm().endpoint().empty() || anthropic))
            completion = completionOf(completionConfig, anthropic);
        // The profiles, for the agent: what llm steps and generative pages name, and understanding's
        std::map<std::string, std::shared_ptr<const llm::Completion>> agentProfiles;
        for (const auto& profile : config.llm_profiles()) {
            if (profile.name().empty()) continue;
            const auto given = profileConfig(config.llm(), profile, completionConfig);
            if (given.endpoint.empty() && !profileIsAnthropic(config.llm(), profile)) {
                spdlog::warn("{} LLM profile \"{}\" has no endpoint: not used", state->logTag, profile.name());
                continue;
            }
            agentProfiles[profile.name()] = std::make_shared<GatedCompletion>(completionOf(given, profileIsAnthropic(config.llm(), profile)),
                                                                             state->llmOffline);
            spdlog::info("{} LLM profile \"{}\": {} ({})", state->logTag, profile.name(), given.endpoint, given.model);
        }
        // With an intent model, the rules match exactly: the model understands the rest.
        std::vector<std::shared_ptr<const nlu::NluProvider>> stages{
            runtime->sentenceEncoder() ? options.agent->exactRules() : options.agent->rules()};
        state->dialog->setExactRules(runtime->sentenceEncoder() != nullptr);
        std::shared_ptr<const nlu::EmbeddingNluProvider> embeddings;
        if (auto encoder = runtime->sentenceEncoder()) {
            embeddings = std::make_shared<nlu::EmbeddingNluProvider>(encoder, options.agent->definition().intents());
            stages.push_back(embeddings);
            if (const auto& cacheFile = config.providers().sentence_encoder_cache(); !cacheFile.empty()) {
                std::filesystem::path cachePath(cacheFile);
                if (cachePath.is_relative() && !runtime->options().modelsDir.empty()) cachePath = std::filesystem::path(runtime->options().modelsDir) / cachePath;
                std::vector<std::string> questions;
                for (const auto& entry : options.agent->definition().knowledge())
                    questions.insert(questions.end(), entry.questions().begin(), entry.questions().end());
                const size_t loaded = embeddings->loadCache(cachePath.string());
                const size_t encodedNow = embeddings->precompute(questions);
                std::string cacheError;
                if (encodedNow > 0 && !embeddings->saveCache(cachePath.string(), &cacheError))
                    spdlog::warn("{} embedding cache: {}", state->logTag, cacheError);
                spdlog::info("{} embeddings: {} read from {}, {} encoded now", state->logTag, loaded, cachePath.string(), encodedNow);
            }
        }
        if (completion) completion = std::make_shared<GatedCompletion>(completion, state->llmOffline);
        // What the agent asks at speed (the intent check, form answers): understanding's profile
        if (const auto& name = config.understanding().llm_profile(); !name.empty()) {
            if (const auto found = agentProfiles.find(name); found != agentProfiles.end()) completion = found->second;
            else spdlog::warn("{} understanding.llm_profile \"{}\" isn't in llm_profiles: llm is asked", state->logTag, name);
        }
        state->dialog->setLlmProfiles(agentProfiles);
        if (completion) {
            const int intentTimeoutMs = config.llm().has_intent_timeout_ms() ? config.llm().intent_timeout_ms() : 1500;
            stages.push_back(std::make_shared<nlu::LlmNluProvider>(completion, options.agent->definition().intents(), intentTimeoutMs));
            state->dialog->setLlm(completion);
        }
        // What finds the agent's entities: the runtime's model, or one of the session's own.
        const auto& entityModel = config.providers().entity_model().id();
        std::shared_ptr<const nlu::EntityExtractor> extractor = runtime->entityExtractor();
        if (entityModel == "tagger") {
            if (auto encoder = runtime->sentenceEncoder()) {
                std::vector<std::string> examples;
                for (const auto& intent : options.agent->definition().intents())
                    examples.insert(examples.end(), intent.examples().begin(), intent.examples().end());
                // shortcut: trained per session (about a second); share it across sessions if many start at once.
                auto tagger = std::make_shared<nlu::TaggerEntityExtractor>(encoder, options.agent->entities().modelLabels(), examples);
                if (tagger->trained()) extractor = tagger;
            } else {
                spdlog::warn("{} entity_model \"tagger\" needs a sentence_encoder: entities by their words alone", state->logTag);
            }
        } else if (entityModel == "llm") {
            if (completion) extractor = std::make_shared<nlu::LlmEntityExtractor>(
                completion, config.llm().has_intent_timeout_ms() ? config.llm().intent_timeout_ms() : 1500);
            else spdlog::warn("{} entity_model \"llm\" needs an LLM: entities by their words alone", state->logTag);
        }
        if (extractor) {
            state->dialog->setEntityExtractor(extractor);
            spdlog::info("{} entities found by {}", state->logTag, extractor->name());
        }
        const auto& understanding = config.understanding();
        if (understanding.has_chat_min_share()) state->dialog->setChatMinShare(understanding.chat_min_share());
        if (stages.size() > 1)
            state->dialog->setNlu(std::make_shared<nlu::CascadeNluProvider>(
                                      std::move(stages), understanding.has_min_confidence() ? understanding.min_confidence() : 0.55f,
                                      understanding.has_keyword_words() ? understanding.keyword_words() : 6.0f),
                                  embeddings);
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
        // An IMMEDIATE trigger cuts off the reply playing, as the cancel button does.
        state->dialog->setInterrupter([raw = state.get()] {
            if (raw->trigger) raw->trigger->trigger(transport::TriggerType::BUTTON_PRESS);
        });
        state->dialog->setReplyObserver([events = std::weak_ptr<EventDispatcher>(state->events), raw = state.get()](
                                            const flow::Reply& reply, const flow::FlowRuntime& conversation) {
            {
                auto saved = conversation.snapshot();
                std::lock_guard<std::mutex> lock(raw->snapshotMutex);
                raw->snapshot = std::move(saved);
            }
            auto dispatcher = events.lock();
            if (!dispatcher) return;
            {
                vasdk::engine::Event event;
                auto* turn = event.mutable_agent_turn();
                turn->set_intent(reply.intent);
                turn->set_matched(reply.matched);
                turn->set_event(reply.event);
                turn->set_flow(conversation.flowName());
                turn->set_page(conversation.pageName());
                turn->set_mid_chat(reply.midChat);
                dispatcher->post(std::move(event));
            }
            for (const auto& trigger : reply.triggers) {
                vasdk::engine::Event event;
                auto* fired = event.mutable_trigger_fired();
                fired->set_name(trigger.name);
                fired->set_decision(trigger.decision);
                fired->set_reason(trigger.reason);
                dispatcher->post(std::move(event));
            }
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
    // Both the LLM and the agent's own LLM calls know everything the assistant said: the LLM's
    // history gets the agent's replies as they pass it (LLMProcessor), and here the rest.
    state->eventTap->setSaidObserver([dialog = std::weak_ptr<flow::DialogProcessor>(state->dialog),
                                      llm = std::weak_ptr<llm::LLMProcessor>(state->llm),
                                      dialogName = state->dialog ? state->dialog->getName() : std::string(),
                                      llmName = state->llm ? state->llm->getName() : std::string()](
                                         const std::string& text, const std::string& source) {
        if (!dialogName.empty() && source == dialogName) return;   // its own: in its transcript, and passed the LLM on the way
        if (auto agent = dialog.lock()) agent->noteSaid(text);   // the LLM's answer, or the app's say()
        if (llmName.empty() || source != llmName)
            if (auto model = llm.lock()) model->noteSaid(text);   // the app's say(): after the LLM in the pipeline
    });
    if (state->llm || state->dialog) {
        state->turnAssembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
        state->turnAssembler->setFirePolicy(std::make_shared<llm::FinalOnlyFirePolicy>());
        // A caller who left takes their unsent words with them.
        auto& audio = *options.transport;
        audio.onDisconnected = [previous = audio.onDisconnected,
                                assembler = std::weak_ptr<pipeline::TurnAssemblerProcessor>(state->turnAssembler)] {
            if (previous) previous();
            if (auto processor = assembler.lock()) processor->reset();
        };
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

// A providers.llm transport (llama.cpp, the app's) reads its instructions from its config, not
// from a prompt template: the system prompt and the tools' descriptions go there, once the app's
// tools are registered. Where the tool prompt carries the whole response contract (formats
// with no tool-call tags, LFM2's tool tokens), a text-shaped format instruction after it
// contradicts it and small models stop calling tools: it's dropped.
void Session::promptProviderLlm() {
    auto& transport = *_state->llmTransport;
    const auto format = transport.getPromptChatFormat();
    const std::string tools = _state->tools->renderToolDescriptions(format);
    auto config = transport.getConfig();
    config.systemPrompt = tools.empty() ? _state->options.systemPrompt : _state->options.systemPrompt + "\n\n" + tools;
    transport.setConfig(config);
    const bool toolPromptOwnsContract = !tools.empty() && (llm::PromptTemplate::create(format)->toolCallTags().first.empty() ||
                                                           format == llm::ChatFormat::LFM2_5 || format == llm::ChatFormat::LFM2);
    if (toolPromptOwnsContract) transport.setResponseFormatInstruction({});
}

bool Session::start(std::string* error) {
    auto fail = [this, error](const std::string& message) {
        spdlog::error("{} {}", _state->logTag, message);
        if (error) *error = message;
        return false;
    };
    if (_state->pipeline) return fail("already started");
    auto& transport = *_state->options.transport;
    if (!transport.initialize() || !transport.start()) return fail("transport " + transport.getName() + " failed to start");
    if (_state->llmFromProvider) promptProviderLlm();
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
    if (const auto& remote = _state->lease.runtime->config().remote_control(); remote.enabled()) {
        std::string reason;
        _state->remote = RemoteControl::start(*this, remote, &reason);
        if (!_state->remote) {
            stop();
            return fail(reason);
        }
    }
    return true;
}

void Session::stop() {
    _state->remote.reset();
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

void Session::fireTrigger(const std::string& name, const google::protobuf::Struct& payload) {
    if (!_state->dialog) {
        spdlog::warn("{} fireTrigger(\"{}\") ignored: no agent", _state->logTag, name);
        return;
    }
    _state->dialog->fireTrigger(name, payload);
}

bool Session::handle(const vasdk::engine::Command& command, std::string* error) {
    switch (command.kind_case()) {
    case vasdk::engine::Command::kInjectText: injectUserText(command.inject_text()); return true;
    case vasdk::engine::Command::kSelectChoice: selectChoice(command.select_choice()); return true;
    case vasdk::engine::Command::kFireTrigger: fireTrigger(command.fire_trigger().name(), command.fire_trigger().payload()); return true;
    case vasdk::engine::Command::kSay: say(command.say()); return true;
    case vasdk::engine::Command::kInterrupt: interrupt(); return true;
    case vasdk::engine::Command::kSetSessionParam:
        setSessionParam(command.set_session_param().name(), command.set_session_param().value());
        return true;
    case vasdk::engine::Command::kResetConversation: resetConversation(); return true;
    case vasdk::engine::Command::kPushToTalk: pushToTalk(command.push_to_talk()); return true;
    case vasdk::engine::Command::kFollowUpListenMs: setFollowUpListenMs(std::max(0, command.follow_up_listen_ms())); return true;
    case vasdk::engine::Command::kSetEntityValues:
        setEntityValues(command.set_entity_values().name(),
                        std::vector<vasdk::nlu::EntityValue>(command.set_entity_values().values().begin(),
                                                             command.set_entity_values().values().end()));
        return true;
    case vasdk::engine::Command::kSetBackchannel: {
        auto current = backchannel();
        applyBackchannel(command.set_backchannel(), current);
        setBackchannel(current);
        return true;
    }
    case vasdk::engine::Command::kEnrollSpeaker:
    case vasdk::engine::Command::kAddSpeaker: {
        const bool done = command.has_add_speaker()
            ? addSpeaker(command.add_speaker().speaker_id(),
                         std::vector<float>(command.add_speaker().embedding().begin(), command.add_speaker().embedding().end()))
            : enrollSpeaker(command.enroll_speaker());
        if (!done && error) *error = "no speaker identification: the config names no providers.speaker_embedding, or no speaker id";
        return done;
    }
    case vasdk::engine::Command::KIND_NOT_SET: break;
    }
    if (error) *error = "an empty command";
    return false;
}

bool Session::enrollSpeaker(const std::string& speakerId) {
    if (!_state->speakerIdentification || speakerId.empty()) return false;
    _state->speakerIdentification->requestEnrollment(speakerId);
    return true;
}

bool Session::addSpeaker(const std::string& speakerId, std::vector<float> voicePrint) {
    if (!_state->speakerIdentification || speakerId.empty() || voicePrint.empty()) return false;
    _state->speakerIdentification->addSpeaker(speakerId, std::move(voicePrint));
    return true;
}

std::map<std::string, std::vector<float>> Session::speakers() const {
    return _state->speakerIdentification ? _state->speakerIdentification->getSpeakers() : std::map<std::string, std::vector<float>>();
}

void Session::resetConversation() {
    interrupt();
    if (_state->llm) _state->llm->clearHistory();
    if (_state->dialog) _state->dialog->restart();
    spdlog::info("{} Conversation started over", _state->logTag);
}

void Session::setBackchannel(const pipeline::BackchannelConfig& backchannel) {
    _state->agentState->setBackchannelConfig(backchannel);
    _state->tts->setPendingPlayback(backchannel.pendingPlaybackMode, backchannel.duckGainDecibels, backchannel.duckDurationMs);
}

pipeline::BackchannelConfig Session::backchannel() const { return _state->agentState->getBackchannelConfig(); }

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

void Session::setNetworkAvailable(bool available) {
    if (available) _state->networkBack();
    else _state->networkLost("network down", true);
}

vasdk::flow::ConversationSnapshot Session::conversationSnapshotMessage() const {
    std::lock_guard<std::mutex> lock(_state->snapshotMutex);
    return _state->snapshot;
}

// Its JSON form, with the proto's field names: {"flow", "page", "session", "form": {"goal", "slots"}}.
std::string Session::conversationSnapshot() const {
    const auto snapshot = conversationSnapshotMessage();
    if (snapshot.flow().empty()) return {};
    google::protobuf::json::PrintOptions options;
    options.preserve_proto_field_names = true;
    std::string json;
    (void)google::protobuf::util::MessageToJsonString(snapshot, &json, options);
    return json;
}

void Session::resumeConversation(const vasdk::flow::ConversationSnapshot& snapshot) {
    if (!_state->dialog) {
        spdlog::warn("{} resumeConversation: no agent", _state->logTag);
        return;
    }
    _state->dialog->setResumeFrom(snapshot);
}

void Session::resumeConversation(const std::string& snapshotJson) {
    vasdk::flow::ConversationSnapshot snapshot;
    google::protobuf::json::ParseOptions options;
    options.ignore_unknown_fields = true;
    if (!google::protobuf::util::JsonStringToMessage(snapshotJson, &snapshot, options).ok()) {
        spdlog::warn("{} resumeConversation: not a snapshot", _state->logTag);
        return;
    }
    resumeConversation(snapshot);
}

std::optional<std::chrono::system_clock::time_point> Session::nextScheduledTrigger() const {
    return _state->dialog ? _state->dialog->nextScheduledTrigger() : std::nullopt;
}

std::string Session::triggerState() const { return _state->dialog ? _state->dialog->triggerState() : std::string(); }

void Session::restoreTriggerState(const std::string& json) {
    if (_state->dialog && !json.empty()) _state->dialog->restoreTriggerState(json);
}

bool Session::usingFallbacks() const {
    std::lock_guard<std::mutex> lock(_state->fallbackMutex);
    return _state->offline;
}

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
