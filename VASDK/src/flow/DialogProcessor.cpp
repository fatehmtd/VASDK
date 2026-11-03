#include "flow/DialogProcessor.h"

#include <spdlog/spdlog.h>

namespace va {
namespace flow {

namespace {
// setConnected() from a transport thread, handled on the processor's own thread.
class ConnectionFrame : public transport::Frame {
public:
    explicit ConnectionFrame(bool connected) : connected(connected) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<ConnectionFrame>(connected); }
    const bool connected;
};
} // namespace

DialogProcessor::DialogProcessor(std::shared_ptr<const CompiledAgent> agent)
    : _conversation(agent), _beforeLastTurn(agent), _agent(agent) {}

bool DialogProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    const auto type = frame->getType();
    return type == transport::FrameType::USER_TURN || type == transport::FrameType::AGENT_STATE ||
           dynamic_cast<const ConnectionFrame*>(frame.get()) != nullptr;
}

void DialogProcessor::setConnected(bool connected) {
    enqueueFrame(std::make_shared<ConnectionFrame>(connected), pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) {
    if (direction != pipeline::FlowDirection::DOWNSTREAM) {
        pushUpstream(frame);
        return;
    }
    if (frame->getType() == transport::FrameType::USER_TURN) {
        handleTurn(frame);
        return;
    }
    if (auto connection = dynamic_cast<const ConnectionFrame*>(frame.get())) {
        if (connection->connected == _connected) return;
        _connected = connection->connected;
        if (!_connected) {
            // The caller left: whoever connects next starts a new conversation.
            spdlog::info("[Dialog] Disconnected - the next conversation starts over");
            _conversation = FlowRuntime(_agent);
            _beforeLastTurn = _conversation;
            _greetingPending = true;
        }
        greetIfReady();
        return;
    }
    pushDownstream(frame);
    const auto& state = static_cast<const transport::AgentStateFrame&>(*frame);
    _agentState = state.currentState;
    // Activated (at start when always on, else by the wake word or button): greet if it's due.
    if (state.previousState == transport::AgentState::IDLE && state.currentState == transport::AgentState::LISTENING)
        greetIfReady();
}

void DialogProcessor::greetIfReady() {
    if (!_greetingPending || !_connected || _agentState == transport::AgentState::IDLE) return;
    _greetingPending = false;
    const auto reply = _conversation.start();
    if (_observer) _observer(reply, _conversation);
    speak(reply);
}

void DialogProcessor::handleTurn(std::shared_ptr<transport::Frame> frame) {
    const auto& turn = static_cast<const transport::UserTurnFrame&>(*frame);
    if (turn.replacesPrevious) _conversation = _beforeLastTurn;
    _beforeLastTurn = _conversation;
    _greetingPending = false;   // the user spoke first: their words are routed on the start page

    const auto reply = _conversation.handleText(turn.text);
    spdlog::info("[Dialog] \"{}\" → {} on {}/{}", turn.text, reply.matched ? "route " + reply.intent : "not understood",
                 _conversation.flowName(), _conversation.pageName());
    if (_observer) _observer(reply, _conversation);
    if (!reply.matched && _passUnmatchedTurns) {
        pushDownstream(frame);
        return;
    }
    if (reply.endsSession) _greetingPending = true;   // on the next activation or caller
    speak(reply);
}

void DialogProcessor::speak(const Reply& reply) {
    if (reply.text.empty()) return;
    auto text = std::make_shared<transport::TextFrame>();
    text->text = reply.text;
    text->isFinal = true;
    text->isExpecting = reply.expectsReply;
    text->sourceId = getName();
    pushDownstream(text);
}

} // namespace flow
} // namespace va
