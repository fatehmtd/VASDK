#pragma once

#include "vasdk_export.h"
#include "../FrameProcessor.h"
#include "FlowRuntime.h"

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
 * a service doesn't hold up the pipeline; its filler is spoken meanwhile. After a question whose
 * answer is private (Expectation.secure_entry, a sensitive slot), the transcript is marked
 * sensitive and the turn never goes on to an LLM; replies carry the values to mask in events.
 */
class VASDK_EXPORT DialogProcessor : public pipeline::FrameProcessor {
public:
    explicit DialogProcessor(std::shared_ptr<const CompiledAgent> agent);

    std::string getName() const override { return "Dialog"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    bool start() override;
    void stop() override;
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override;

    /** With an LLM after this stage: turns no route understands go on to it instead of the
     *  not-understood reply. Before start(). */
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
    /** How call_tool steps reach tools (FlowRuntime::setToolCaller). Before start(). */
    void setToolCaller(ToolCaller caller);

private:
    void greetIfReady();
    void speak(const Reply& reply, bool filler = false);
    // The conversation's thread: tasks run in order, and post their replies back as frames.
    void post(std::function<void()> task);
    void deliver(Reply reply, std::shared_ptr<transport::Frame> turn = nullptr, bool mayPassOn = false);
    void conversationLoop();
    void prepare(FlowRuntime& conversation);
    void armNoInput(int ms);
    void disarmNoInput();
    void noInputTimerLoop();

    // Used only on the conversation's thread.
    FlowRuntime _conversation;
    FlowRuntime _beforeLastTurn;   // restored when the user resumes the last turn
    std::shared_ptr<const CompiledAgent> _agent;
    ToolCaller _toolCaller;
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
    bool _passUnmatchedTurns = false;
    ReplyObserver _observer;
    FlowRuntime::ContextSource _contextSource;
    bool _awaitingAnswer = false;   // the last reply asked a question
    bool _replyPlaying = false;     // a reply of the flow is being spoken
    // The no-input timer: a deadline, and a generation that makes a fired timer stale once moved on.
    std::thread _timerThread;
    std::mutex _timerMutex;
    std::condition_variable _timerChanged;
    std::optional<std::chrono::steady_clock::time_point> _noInputDeadline;
    uint64_t _noInputGeneration = 0;
    bool _timerStop = false;
};

} // namespace flow
} // namespace va
