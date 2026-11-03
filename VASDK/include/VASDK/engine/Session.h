#pragma once

#include "vasdk_export.h"
#include "../AgentStateProcessor.h"
#include "../metrics/MetricsConfig.h"
#include "EventTapProcessor.h"

#include <memory>
#include <string>
#include <vector>

namespace google::protobuf { class Value; }
namespace vasdk::nlu { class EntityValue; }

namespace va {

class AudioRingBuffer;
namespace context { class ContextProvider; }
namespace flow { class CompiledAgent; }
namespace llm { class LLMTransport; class Tool; class ToolDispatchProcessor; }
namespace metrics { class MetricsReporter; }
namespace pipeline { class FrameProcessor; class Pipeline; }
namespace transport { class AudioTransport; }

namespace engine {

class Runtime;

/**
 * @brief One conversation: the standard pipeline over a Runtime's providers.
 *
 *   input → trigger → [wake word] → [AfterInput] → VAD → turn → STT → [AfterTranscription]
 *     → agent state → turn assembly → dialog → LLM → tools → [BeforeSynthesis] → TTS → [BeforeOutput] → output
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
        AfterTranscription,   ///< transcriptions, before the agent reacts (speaker id, logging)
        BeforeSynthesis,      ///< reply text, before TTS
        BeforeOutput,         ///< synthesized audio, before the transport plays it
    };

    struct Options {
        /** Required: mic in, speech out. The session configures, starts and stops it. */
        std::shared_ptr<transport::AudioTransport> transport;
        /** Names the session in logs and in every event's session_id, e.g. a call id. */
        std::string id;
        std::string systemPrompt = "You are a helpful voice assistant. Keep responses brief and conversational.";
        /** Appended to every LLM request: how the reply must be formatted. */
        std::string responseFormatInstruction;
        /** Use this LLM transport (a local model, say) instead of the config's `llm` endpoint.
         *  It holds the conversation history, so give each session its own. */
        std::shared_ptr<llm::LLMTransport> llmTransport;
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
    /** Stops the assistant's current reply, like a barge-in. Thread-safe. */
    void interrupt();
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

    const std::string& id() const;
    bool hasLLM() const;
    /** "TransportInputProcessor → TriggerProcessor → VADFrameProcessor (SileroVAD) → ..." */
    std::string describe() const;

    /** Mic audio, for app processors that read it directly (recorders, speaker id). */
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
    std::unique_ptr<State> _state;
};

} // namespace engine
} // namespace va
