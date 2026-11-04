#pragma once

#include "vasdk_export.h"
#include "../FrameProcessor.h"
#include "../metrics/MetricHandles.h"
#include "FlowRuntime.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace va {
namespace flow {

/**
 * @brief Answers user turns from an agent: each UserTurnFrame is routed on the current page and
 *        the reply goes downstream as a TextFrame (to TTS). Place it after the turn assembler.
 *
 * The start page's greeting is spoken once the assistant is listening (at start when always on,
 * else on the wake word or button) and someone is there to hear it (setConnected), and again
 * after the conversation ended. When the caller leaves, the next one starts a new conversation.
 * A turn the user resumed (UserTurnFrame::replacesPrevious) undoes the previous one first.
 * After a question has played, silence for the page's no_input_ms raises no-input
 * (FlowRuntime::handleNoInput); the user starting to speak cancels it.
 * Each reply also goes upstream as a FollowUpListenFrame, for how long AgentStateProcessor keeps
 * the mic open after it, and its final TextFrame carries what the user is expected to say.
 *
 * Turns are answered on a thread of the processor's own, one at a time, so a call_tool waiting on
 * a service doesn't hold up the pipeline; its filler is spoken meanwhile. So does an LLM that
 * takes over a second (FlowRuntime::setLlmFillerAfterMs). A turn the user resumes, a restart, a
 * disconnect or stop() give up a turn still waiting for the LLM: its reply isn't spoken. After a question whose
 * answer is private (Expectation.secure_entry, a sensitive slot), the transcript is marked
 * sensitive and the turn never goes on to an LLM; replies carry the values to mask in events.
 */
class VASDK_EXPORT DialogProcessor : public pipeline::FrameProcessor {
public:
    explicit DialogProcessor(std::shared_ptr<const CompiledAgent> agent);

    std::string getName() const override { return "Dialog"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    /** With a metrics collector: flow_turns, flow_no_match, flow_no_input, flow_max_reprompts,
     *  flow_tool_errors, flow_clarify, flow_page_changes, flow_sessions_ended, flow_passed_to_llm,
     *  flow_triggers_said (counters) and flow_turn_ms (time to answer a turn). */
    bool initialize() override;
    bool start() override;
    void stop() override;
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override;

    /** With an LLM after this stage: turns no route understands go on to it instead of the
     *  not-understood reply. Thread-safe. */
    void setPassUnmatchedTurns(bool pass) { _passUnmatchedTurns = pass; }
    using ReplyObserver = std::function<void(const Reply& reply, const FlowRuntime& conversation)>;
    /** Called with every reply (page changes, tool calls, the end of the conversation) and where
     *  the conversation is after it, on the conversation's thread: keep it quick. Before start(). */
    void setReplyObserver(ReplyObserver observer) { _observer = std::move(observer); }
    /**
     * Whether someone is connected to hear the assistant (a browser peer, a phone call). Until
     * then the greeting waits; on a disconnect the conversation starts over for the next one.
     * Default: connected. Thread-safe: call it from the transport's callbacks.
     */
    void setConnected(bool connected);
    /** Sets a session param, as FlowRuntime::setSessionParam; errors are logged. Thread-safe. */
    void setSessionParam(const std::string& name, const Value& value);
    /** Replaces a dynamic entity's values, as FlowRuntime::setEntityValues; errors are logged. Thread-safe. */
    void setEntityValues(const std::string& name, std::vector<vasdk::nlu::EntityValue> values);
    /** Starts the conversation over at the start page, params at their defaults; the greeting
     *  comes on the next activation. Thread-safe. */
    void restart();
    /** The user tapped the last reply's choice at `index`, from 0 (FlowRuntime::selectChoice). Thread-safe. */
    void selectChoice(int index);
    /** What $context.* reads; called on the conversation's thread, so it must be thread-safe. Before start(). */
    void setContextSource(FlowRuntime::ContextSource source);
    /** What understands intents, and what answers knowledge questions by meaning (FlowRuntime::setNlu, setEmbeddings). Before start(). */
    void setNlu(std::shared_ptr<const nlu::NluProvider> provider, std::shared_ptr<const nlu::EmbeddingNluProvider> embeddings = nullptr);
    /** What `llm` steps, generative pages and unread form answers ask (FlowRuntime::setLlm). Before start(). */
    void setLlm(std::shared_ptr<const llm::Completion> completion);
    /** How call_tool steps reach tools (FlowRuntime::setToolCaller). Before start(). */
    void setToolCaller(ToolCaller caller);
    /**
     * Fires an agent trigger by its app_event name or its own (FlowRuntime::fireTrigger). It
     * waits as its delivery says: AFTER_TURN ones until the reply playing has ended, AFTER_FLOW
     * ones until the conversation is between tasks. Thread-safe.
     */
    void fireTrigger(const std::string& name, const google::protobuf::Struct& payload = {});

private:
    void greetIfReady();
    void speak(const Reply& reply, bool filler = false);
    // The conversation's thread: tasks run in order, and post their replies back as frames.
    void post(std::function<void()> task);
    void deliver(Reply reply, std::shared_ptr<transport::Frame> turn = nullptr, bool mayPassOn = false, bool fromTrigger = false);
    void conversationLoop();
    void prepare(FlowRuntime& conversation);
    // Two timers on one thread: no-input after a question, and the agent's inactivity triggers.
    struct Timer {
        std::optional<std::chrono::steady_clock::time_point> deadline;
        uint64_t generation = 0;   // a fired timer is stale once this moved on
    };
    void arm(Timer& timer, int ms);
    void disarm(Timer& timer);
    void armNoInput(int ms) { arm(_noInput, ms); }
    void disarmNoInput() { disarm(_noInput); }
    void armInactivity();
    void noInputTimerLoop();

    // Used only on the conversation's thread.
    FlowRuntime _conversation;
    FlowRuntime _beforeLastTurn;   // restored when the user resumes the last turn
    std::shared_ptr<const CompiledAgent> _agent;
    ToolCaller _toolCaller;
    std::shared_ptr<const nlu::NluProvider> _nlu;
    std::shared_ptr<const nlu::EmbeddingNluProvider> _embeddings;
    std::shared_ptr<const llm::Completion> _llm;
    std::thread _conversationThread;
    std::mutex _tasksMutex;
    std::condition_variable _tasksChanged;
    std::deque<std::function<void()>> _tasks;
    bool _tasksStop = false;
    // Used only on the pipeline thread.
    int _noInputMs = 0;             // the page's no-input wait after the last reply
    bool _privateAnswer = false;    // the last question's answer is private
    bool _greetingPending = true;
    transport::AgentState _agentState = transport::AgentState::IDLE;
    bool _connected = true;
    std::atomic<bool> _passUnmatchedTurns{false};   // off while the network is down
    bool _llmTookLastTurn = false;                   // on the conversation's thread
    // Turns are numbered as they come; those below _givenUpBefore stop waiting for the LLM.
    std::atomic<uint64_t> _turnsSeen{0};
    std::atomic<uint64_t> _givenUpBefore{0};
    uint64_t _answering = 0;                         // the turn being answered, on the conversation's thread
    void giveUpTurns() { _givenUpBefore = ++_turnsSeen; }
    ReplyObserver _observer;
    FlowRuntime::ContextSource _contextSource;
    bool _awaitingAnswer = false;   // the last reply asked a question
    bool _replyPlaying = false;     // a reply of the flow is being spoken
    // The no-input timer: a deadline, and a generation that makes a fired timer stale once moved on.
    std::thread _timerThread;
    std::mutex _timerMutex;
    std::condition_variable _timerChanged;
    Timer _noInput;
    Timer _inactivity;
    Timer _tick;                    // every second, for schedule and context_change triggers
    bool _timedTriggers = false;
    bool _timerStop = false;
    // The agent's inactivity triggers, shortest first; the next to fire since the user last spoke.
    std::vector<std::pair<std::string, int>> _inactivityTriggers;
    size_t _nextInactivity = 0;
    bool _userSpeaking = false;
    // Flow metrics (with a collector).
    struct FlowMetrics {
        metrics::CounterHandle turns, noMatch, noInput, maxReprompts, toolErrors, clarify, pageChanges, sessionsEnded,
            passedToLlm, triggersSaid;
        metrics::SamplerHandle turnMs;
    } _flowMetrics;
    void count(const Reply& reply);
};

} // namespace flow
} // namespace va
