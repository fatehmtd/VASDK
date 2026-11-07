#pragma once

#include "vasdk_export.h"
#include "../AgentStateProcessor.h"
#include "../metrics/MetricsConfig.h"
#include "EventTapProcessor.h"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <google/protobuf/struct.pb.h>
namespace vasdk::nlu { class EntityValue; }
namespace vasdk::engine { class Command; }
namespace vasdk::flow { class ConversationSnapshot; }

namespace va {

class AudioRingBuffer;
namespace context { class ContextProvider; }
namespace flow { class CompiledAgent; }
namespace llm { class Completion; class LLMTransport; class Tool; class ToolDispatchProcessor; }
namespace metrics { class MetricsReporter; }
namespace pipeline { class FrameProcessor; class Pipeline; }
namespace transport { class AudioTransport; }

namespace engine {

class Runtime;

/**
 * @brief One conversation: the standard pipeline over a Runtime's providers.
 *
 *   input → [audio DSP] → trigger → [wake word] → [AfterInput] → VAD → turn → STT → [speaker id]
 *     → [AfterTranscription] → agent state → turn assembly → dialog → LLM → tools → [BeforeSynthesis]
 *     → TTS → [BeforeOutput] → output
 *
 * The audio DSP is there when the config names providers.audio_dsp: every stage after it hears
 * its output. Speaker identification is there with providers.speaker_embedding: each utterance
 * ends in a speaker_identified event.
 *
 * The dialog stage is there when Options::agent is set: the agent's pages answer each turn,
 * and turns no route understands go on to the LLM when there is one. The LLM stages are there
 * only when the config has an `llm` endpoint (or Options::llmTransport is set); with neither,
 * the session still speaks say() text.
 * What happens (transcripts, replies, state changes, tool calls, errors) reaches
 * listeners added with addListener().
 *
 * Get one from Runtime::createSession(), or use VAEngine for a single conversation.
 * Destroying the session stops it and hands its providers back to the runtime.
 */
class VASDK_EXPORT Session {
public:
    /** Where addProcessor() puts an app processor in the standard pipeline. */
    enum class Slot {
        AfterInput,           ///< raw mic audio, before VAD (recorders, visualisers)
        AfterTranscription,   ///< transcriptions, before the agent reacts (logging)
        BeforeSynthesis,      ///< reply text, before TTS
        BeforeOutput,         ///< synthesized audio, before the transport plays it
    };

    struct Options {
        /** Required: mic in, speech out. The session configures, starts and stops it. */
        std::shared_ptr<transport::AudioTransport> transport;
        /** Names the session in logs and in every event's session_id, e.g. a call id. */
        std::string id;
        /** The LLM's instructions. `{context}` in them is replaced, on every request, by `context`
         *  as JSON as it is then. */
        std::string systemPrompt = "You are a helpful voice assistant. Keep responses brief and conversational.";
        /** Appended to every LLM request: how the reply must be formatted. */
        std::string responseFormatInstruction;
        /** Use this LLM transport (a local model, say) instead of the config's `llm` endpoint.
         *  It holds the conversation history, so give each session its own. */
        std::shared_ptr<llm::LLMTransport> llmTransport;
        /** What the agent asks an LLM: intents its rules aren't sure of, `llm` steps, generative
         *  pages. Unset: built from the config's `llm` endpoint, when there is one. */
        std::shared_ptr<const llm::Completion> llmCompletion;
        /** Answer turns from this agent's pages (CompiledAgent::load), before any LLM. Sessions
         *  may share one agent; each keeps its own place in it. */
        std::shared_ptr<const flow::CompiledAgent> agent;
        /** What the agent's $context.* references read: device state, location, app data. */
        std::shared_ptr<context::ContextProvider> context;
        /** Send every frame to the transport, not just speech audio: for a UI that shows state (WebRTC page). */
        bool sendAllFramesToTransport = false;
        pipeline::BackchannelConfig backchannel;
        /** Optional: bind every processor's metrics to this reporter. */
        std::shared_ptr<metrics::MetricsReporter> metricsReporter;
        metrics::MetricsConfig metricsConfig;
    };

    virtual ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /** Adds an app processor at `slot`, after any added there before. Before start() only. */
    bool addProcessor(Slot slot, std::shared_ptr<pipeline::FrameProcessor> processor);
    /** Makes a tool available to the LLM. Can be called before start(). */
    void registerTool(const llm::Tool& tool);

    /**
     * Calls `listener` with every vasdk::engine::Event, in order, on the session's event thread
     * (never a pipeline thread): keep it quick. Any time, before or after start().
     * Returns an id for removeListener().
     */
    int addListener(EventListener listener);
    void removeListener(int id);

    /** Starts the transport and the pipeline. */
    bool start(std::string* error = nullptr);
    void stop();

    /** Speaks `text` as the assistant. Thread-safe. */
    void say(const std::string& text);
    /** Sends `text` as a finished user turn, as if spoken. Needs an LLM or an agent. Thread-safe. */
    void injectUserText(const std::string& text);
    /**
     * The user tapped the choice at `index` (from 0) of the agent's last reply, its
     * AgentReply.choices: the same as saying its value. Thread-safe; needs Options::agent (for an
     * LLM's choices, send the label with injectUserText).
     */
    void selectChoice(int index);
    /** Stops the assistant's current reply, like a barge-in. Thread-safe. */
    void interrupt();
    /**
     * Fires one of the agent's triggers, by its app_event name or its own, with what its
     * $trigger.payload reads: "car-ready" with {"booking": "B-42"}. Its delivery decides when it's
     * said (a trigger_fired event tells). Thread-safe; needs Options::agent.
     */
    void fireTrigger(const std::string& name, const google::protobuf::Struct& payload = {});
    /**
     * Does what a vasdk.engine.Command asks (inject_text, select_choice, fire_trigger, say,
     * interrupt, set_session_param, reset_conversation, push_to_talk, follow_up_listen_ms,
     * set_entity_values, set_backchannel, enroll_speaker, add_speaker): what remote_control's
     * POST /command and the C API's vasdk_session_send take. False, with `error` set, for an empty
     * one or one the session can't do (enroll_speaker without providers.speaker_embedding). Thread-safe.
     */
    bool handle(const vasdk::engine::Command& command, std::string* error = nullptr);
    /**
     * Starts the conversation over: the reply is cut, the LLM's history dropped, and an agent goes
     * back to its start page with its session params at their defaults (greeting on the next
     * activation). Thread-safe.
     */
    void resetConversation();
    /** Changes how speech over the assistant is handled (back-channels, ducking). Thread-safe; applies from the next frame. */
    void setBackchannel(const pipeline::BackchannelConfig& backchannel);
    /** How speech over the assistant is handled now: the config's, Options::backchannel, or setBackchannel()'s. */
    pipeline::BackchannelConfig backchannel() const;
    /**
     * The push-to-talk button: true while held, false on release. Listens while held (a reply
     * in progress is cut); releasing stops listening unless the user is mid-sentence, whose turn
     * then gets its answer. Thread-safe.
     */
    void pushToTalk(bool held);
    /**
     * How long the mic stays open after each reply for the user to answer without the wake
     * phrase or button, in ms: overrides the config's activation.follow_up.listen_ms (default
     * 8000). 0 closes it at once, -1 never. Thread-safe; applies from the next reply.
     */
    void setFollowUpListenMs(int ms);
    /**
     * Sets an agent session param ($session.<name>), e.g. the caller's tier from a CRM. It must
     * be declared in the agent; a mismatched type is logged. Applies from the next turn; kept
     * until the caller disconnects. Thread-safe; needs Options::agent.
     */
    void setSessionParam(const std::string& name, const google::protobuf::Value& value);
    /**
     * Replaces the values of a `dynamic` list entity for this conversation, e.g. the appointment
     * times still free today, or the caller's cars. Applies from the next turn; kept until the
     * caller disconnects. Thread-safe; needs Options::agent.
     */
    void setEntityValues(const std::string& name, const std::vector<vasdk::nlu::EntityValue>& values);
    /**
     * Where the agent's conversation is after its last reply (flow::FlowRuntime::snapshot): its
     * page, the form's filled slots and the session params, sensitive ones left out. Store it
     * when a caller hangs up mid-task, and give it to resumeConversation() when they call back.
     * Empty without an agent, before the first reply, or once the conversation ended. Thread-safe.
     */
    std::string conversationSnapshot() const;
    /** The same, as the vasdk.flow.ConversationSnapshot itself (its flow empty when there's none). */
    vasdk::flow::ConversationSnapshot conversationSnapshotMessage() const;
    /**
     * Before start(): the agent's greeting picks the conversation up where `snapshotJson` left it
     * (flow::FlowRuntime::resume) instead of at its start page. Needs Options::agent.
     */
    void resumeConversation(const std::string& snapshotJson);
    void resumeConversation(const vasdk::flow::ConversationSnapshot& snapshot);
    /**
     * The agent's schedule triggers when the app may be closed: when the next one is due (set an
     * alarm for it: Android's AlarmManager), and their state to keep across restarts, given back
     * with restoreTriggerState() so one that came due while closed fires at the first check and
     * one already said isn't said again. Without an agent: none, "" and a no-op. Thread-safe.
     */
    std::optional<std::chrono::system_clock::time_point> nextScheduledTrigger() const;
    std::string triggerState() const;
    void restoreTriggerState(const std::string& json);

    /**
     * Whether the network is up, as the app sees it (Android's ConnectivityManager, a request
     * that failed). Down: STT and TTS switch to the config's `fallback` providers between turns
     * (loaded in the background the first time) and the LLM isn't asked, so the agent answers
     * on its own. Up: back to the configured providers and the LLM. An STT or TTS provider
     * error switches to its fallbacks too. Each change is a `provider_switched` event. Thread-safe.
     */
    void setNetworkAvailable(bool available);
    /** True while the network is down (setNetworkAvailable) or a provider failed. */
    bool usingFallbacks() const;

    /**
     * The next utterance long enough to identify enrolls `speakerId`, replacing their voice print;
     * a speaker_enrollment event says how it went. False without providers.speaker_embedding. Thread-safe.
     */
    bool enrollSpeaker(const std::string& speakerId);
    /** Restores a voice print kept from speakers(). False without providers.speaker_embedding. Thread-safe. */
    bool addSpeaker(const std::string& speakerId, std::vector<float> voicePrint);
    /** The enrolled speakers' voice prints (L2-normalized), to keep across sessions. Empty without providers.speaker_embedding. */
    std::map<std::string, std::vector<float>> speakers() const;

    const std::string& id() const;
    bool hasLLM() const;
    /** "TransportInputProcessor → TriggerProcessor → VADFrameProcessor (SileroVAD) → ..." */
    std::string describe() const;

    /** Mic audio as every stage hears it (after providers.audio_dsp, when there is one), for app
     *  processors that read it directly (recorders, visualisers). */
    std::shared_ptr<AudioRingBuffer> inputRingBuffer() const;
    std::shared_ptr<llm::LLMTransport> llmTransport() const;
    /** The tools the LLM can call, for code that adds or swaps them as the call goes on (goals). */
    std::shared_ptr<llm::ToolDispatchProcessor> toolDispatcher() const;
    /** nullptr before start(). */
    std::shared_ptr<pipeline::Pipeline> pipeline() const;

protected:
    /** Takes a provider set from `runtime`; check created() after: false, with `error` set, when
     *  the runtime has none to spare. */
    Session(std::shared_ptr<Runtime> runtime, Options options, std::string* error);
    bool created() const { return _state != nullptr; }

private:
    friend class Runtime;
    struct State;
    static std::unique_ptr<State> createState(std::shared_ptr<Runtime> runtime, Options options, std::string* error);
    void promptProviderLlm();
    std::unique_ptr<State> _state;
};

} // namespace engine
} // namespace va
