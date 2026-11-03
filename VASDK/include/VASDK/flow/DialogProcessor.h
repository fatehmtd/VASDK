#pragma once

#include "vasdk_export.h"
#include "../FrameProcessor.h"
#include "FlowRuntime.h"

#include <functional>
#include <memory>

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
 */
class VASDK_EXPORT DialogProcessor : public pipeline::FrameProcessor {
public:
    explicit DialogProcessor(std::shared_ptr<const CompiledAgent> agent);

    std::string getName() const override { return "Dialog"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override;

    /** With an LLM after this stage: turns no route understands go on to it instead of the
     *  not-understood reply. Before start(). */
    void setPassUnmatchedTurns(bool pass) { _passUnmatchedTurns = pass; }
    using ReplyObserver = std::function<void(const Reply& reply, const FlowRuntime& conversation)>;
    /** Called with every reply (page changes, the end of the conversation) and where the
     *  conversation is after it, on the pipeline thread: keep it quick. Before start(). */
    void setReplyObserver(ReplyObserver observer) { _observer = std::move(observer); }
    /**
     * Whether someone is connected to hear the assistant (a browser peer, a phone call). Until
     * then the greeting waits; on a disconnect the conversation starts over for the next one.
     * Default: connected. Thread-safe: call it from the transport's callbacks.
     */
    void setConnected(bool connected);
    /** Sets a session param, as FlowRuntime::setSessionParam; errors are logged. Thread-safe. */
    void setSessionParam(const std::string& name, const Value& value);
    /** What $context.* reads; called on the pipeline thread, so it must be thread-safe. Before start(). */
    void setContextSource(FlowRuntime::ContextSource source);

private:
    void handleTurn(std::shared_ptr<transport::Frame> frame);
    void greetIfReady();
    void speak(const Reply& reply);

    FlowRuntime _conversation;
    FlowRuntime _beforeLastTurn;   // restored when the user resumes the last turn
    std::shared_ptr<const CompiledAgent> _agent;
    bool _greetingPending = true;
    transport::AgentState _agentState = transport::AgentState::IDLE;
    bool _connected = true;
    bool _passUnmatchedTurns = false;
    ReplyObserver _observer;
    FlowRuntime::ContextSource _contextSource;
};

} // namespace flow
} // namespace va
