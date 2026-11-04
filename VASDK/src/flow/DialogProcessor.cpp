#include "flow/DialogProcessor.h"

#include <google/protobuf/util/json_util.h>
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

// setSessionParam() from an app thread, applied on the processor's own thread.
class SessionParamFrame : public transport::Frame {
public:
    SessionParamFrame(std::string name, Value value) : name(std::move(name)), value(std::move(value)) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<SessionParamFrame>(name, value); }
    const std::string name;
    const Value value;
};

// setEntityValues() from an app thread, applied on the processor's own thread.
class EntityValuesFrame : public transport::Frame {
public:
    EntityValuesFrame(std::string name, std::vector<vasdk::nlu::EntityValue> values)
        : name(std::move(name)), values(std::move(values)) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<EntityValuesFrame>(name, values); }
    const std::string name;
    const std::vector<vasdk::nlu::EntityValue> values;
};

// selectChoice() from an app thread, handled on the processor's own thread.
class ChoiceFrame : public transport::Frame {
public:
    explicit ChoiceFrame(int index) : index(index) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<ChoiceFrame>(index); }
    const int index;
};

// After a question, the mic stays open past the no-input timer, so its reprompt is heard.
// shortcut: a fixed allowance for the reprompt to start playing; measure TTS start-up if it's too short.
constexpr int kNoInputReplyMarginMs = 2000;

// The no-input timer fired, handled on the processor's own thread.
class NoInputFrame : public transport::Frame {
public:
    explicit NoInputFrame(uint64_t generation) : generation(generation) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<NoInputFrame>(generation); }
    const uint64_t generation;
};

// A reply from the conversation's thread, spoken on the processor's own.
class ReplyFrame : public transport::Frame {
public:
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<ReplyFrame>(*this); }
    Reply reply;
    int noInputMs = 0;
    bool filler = false;                       // said while a tool runs; the turn's reply follows
    std::shared_ptr<transport::Frame> turn;    // the user turn it answers
    bool mayPassOn = false;                    // not understood, and not private: an LLM may take it
};
} // namespace

DialogProcessor::DialogProcessor(std::shared_ptr<const CompiledAgent> agent)
    : _conversation(agent), _beforeLastTurn(agent), _agent(agent) {
    prepare(_conversation);
}

void DialogProcessor::prepare(FlowRuntime& conversation) {
    conversation.setContextSource(_contextSource);
    if (_toolCaller) conversation.setToolCaller(_toolCaller);
    conversation.setFillerSink([this](const Reply& sofar) {
        auto frame = std::make_shared<ReplyFrame>();
        frame->reply = sofar;
        frame->filler = true;
        enqueueFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    });
}

bool DialogProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    const auto type = frame->getType();
    return type == transport::FrameType::USER_TURN || type == transport::FrameType::AGENT_STATE ||
           type == transport::FrameType::TRANSCRIPTION ||
           dynamic_cast<const ConnectionFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const SessionParamFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const EntityValuesFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const ChoiceFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const NoInputFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const ReplyFrame*>(frame.get()) != nullptr;
}

bool DialogProcessor::start() {
    {
        std::lock_guard<std::mutex> lock(_timerMutex);
        _timerStop = false;
    }
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        _tasksStop = false;
    }
    if (!_timerThread.joinable()) _timerThread = std::thread(&DialogProcessor::noInputTimerLoop, this);
    if (!_conversationThread.joinable()) _conversationThread = std::thread(&DialogProcessor::conversationLoop, this);
    return FrameProcessor::start();
}

void DialogProcessor::stop() {
    {
        std::lock_guard<std::mutex> lock(_timerMutex);
        _timerStop = true;
    }
    _timerChanged.notify_all();
    if (_timerThread.joinable()) _timerThread.join();
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        _tasksStop = true;
    }
    _tasksChanged.notify_all();
    if (_conversationThread.joinable()) _conversationThread.join();
    FrameProcessor::stop();
}

void DialogProcessor::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        _tasks.push_back(std::move(task));
    }
    _tasksChanged.notify_one();
}

void DialogProcessor::conversationLoop() {
    std::unique_lock<std::mutex> lock(_tasksMutex);
    while (true) {
        _tasksChanged.wait(lock, [this] { return _tasksStop || !_tasks.empty(); });
        if (_tasksStop) return;   // what's left would be spoken after the pipeline stopped
        auto task = std::move(_tasks.front());
        _tasks.pop_front();
        lock.unlock();
        task();
        lock.lock();
    }
}

void DialogProcessor::deliver(Reply reply, std::shared_ptr<transport::Frame> turn, bool mayPassOn) {
    if (_observer) _observer(reply, _conversation);
    auto frame = std::make_shared<ReplyFrame>();
    frame->reply = std::move(reply);
    frame->noInputMs = _conversation.noInputMs();
    frame->turn = std::move(turn);
    frame->mayPassOn = mayPassOn;
    enqueueFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::armNoInput(int ms) {
    std::lock_guard<std::mutex> lock(_timerMutex);
    ++_noInputGeneration;
    _noInputDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    _timerChanged.notify_all();
}

void DialogProcessor::disarmNoInput() {
    std::lock_guard<std::mutex> lock(_timerMutex);
    if (!_noInputDeadline) return;
    ++_noInputGeneration;
    _noInputDeadline.reset();
    _timerChanged.notify_all();
}

void DialogProcessor::noInputTimerLoop() {
    std::unique_lock<std::mutex> lock(_timerMutex);
    while (!_timerStop) {
        if (!_noInputDeadline) {
            _timerChanged.wait(lock);
            continue;
        }
        const auto deadline = *_noInputDeadline;
        const auto generation = _noInputGeneration;
        if (_timerChanged.wait_until(lock, deadline) == std::cv_status::timeout && generation == _noInputGeneration) {
            _noInputDeadline.reset();
            lock.unlock();
            enqueueFrame(std::make_shared<NoInputFrame>(generation), pipeline::FlowDirection::DOWNSTREAM);
            lock.lock();
        }
    }
}

void DialogProcessor::setSessionParam(const std::string& name, const Value& value) {
    enqueueFrame(std::make_shared<SessionParamFrame>(name, value), pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::setEntityValues(const std::string& name, std::vector<vasdk::nlu::EntityValue> values) {
    enqueueFrame(std::make_shared<EntityValuesFrame>(name, std::move(values)), pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::selectChoice(int index) {
    enqueueFrame(std::make_shared<ChoiceFrame>(index), pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::setContextSource(FlowRuntime::ContextSource source) {
    _contextSource = std::move(source);
    _conversation.setContextSource(_contextSource);
    _beforeLastTurn.setContextSource(_contextSource);
}

void DialogProcessor::setToolCaller(ToolCaller caller) {
    _toolCaller = std::move(caller);
    _conversation.setToolCaller(_toolCaller);
    _beforeLastTurn.setToolCaller(_toolCaller);
}

void DialogProcessor::setConnected(bool connected) {
    enqueueFrame(std::make_shared<ConnectionFrame>(connected), pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) {
    if (direction != pipeline::FlowDirection::DOWNSTREAM) {
        pushUpstream(frame);
        return;
    }
    if (frame->getType() == transport::FrameType::TRANSCRIPTION) {
        if (_privateAnswer) std::static_pointer_cast<transport::TranscriptionFrame>(frame)->sensitive = true;
        pushDownstream(frame);
        return;
    }
    if (auto ready = std::dynamic_pointer_cast<ReplyFrame>(frame)) {
        if (ready->filler) {
            speak(ready->reply, true);
            return;
        }
        _noInputMs = ready->noInputMs;
        if (ready->mayPassOn && _passUnmatchedTurns) {
            pushDownstream(ready->turn);
            return;
        }
        if (ready->reply.endsSession) _greetingPending = true;   // on the next activation or caller
        speak(ready->reply);
        return;
    }
    if (frame->getType() == transport::FrameType::USER_TURN) {
        disarmNoInput();
        _greetingPending = false;   // the user spoke first: their words are routed on the start page
        const bool privateAnswer = _privateAnswer;
        post([this, frame, privateAnswer] {
            const auto& turn = static_cast<const transport::UserTurnFrame&>(*frame);
            if (turn.replacesPrevious) _conversation = _beforeLastTurn;
            _beforeLastTurn = _conversation;
            auto reply = _conversation.handleText(turn.text);
            spdlog::info("[Dialog] \"{}\" → {} on {}/{}", privateAnswer ? maskValue(turn.text) : redact(turn.text, reply.sensitiveValues),
                         reply.matched ? "route " + reply.intent : "not understood", _conversation.flowName(), _conversation.pageName());
            const bool mayPassOn = !reply.matched && !privateAnswer;   // a private answer never goes to an LLM
            deliver(std::move(reply), frame, mayPassOn);
        });
        return;
    }
    if (auto noInput = dynamic_cast<const NoInputFrame*>(frame.get())) {
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            if (noInput->generation != _noInputGeneration) return;   // moved on since it fired
        }
        post([this] {
            _beforeLastTurn = _conversation;
            auto reply = _conversation.handleNoInput();
            spdlog::info("[Dialog] no input → {} on {}/{}", reply.event, _conversation.flowName(), _conversation.pageName());
            deliver(std::move(reply));
        });
        return;
    }
    if (auto choice = dynamic_cast<const ChoiceFrame*>(frame.get())) {
        disarmNoInput();
        _greetingPending = false;
        post([this, index = choice->index] {
            _beforeLastTurn = _conversation;
            auto reply = _conversation.selectChoice(index);
            spdlog::info("[Dialog] choice {} → {} on {}/{}", index, reply.matched ? "route " + reply.intent : "not understood",
                         _conversation.flowName(), _conversation.pageName());
            deliver(std::move(reply));
        });
        return;
    }
    if (auto param = std::dynamic_pointer_cast<SessionParamFrame>(frame)) {
        post([this, param] {
            std::string error;
            if (!_conversation.setSessionParam(param->name, param->value, &error)) spdlog::error("[Dialog] {}", error);
        });
        return;
    }
    if (auto entityValues = std::dynamic_pointer_cast<EntityValuesFrame>(frame)) {
        post([this, entityValues] {
            std::string error;
            if (!_conversation.setEntityValues(entityValues->name, entityValues->values, &error)) spdlog::error("[Dialog] {}", error);
        });
        return;
    }
    if (auto connection = dynamic_cast<const ConnectionFrame*>(frame.get())) {
        if (connection->connected == _connected) return;
        _connected = connection->connected;
        if (!_connected) {
            // The caller left: whoever connects next starts a new conversation.
            spdlog::info("[Dialog] Disconnected - the next conversation starts over");
            post([this] {
                _conversation = FlowRuntime(_agent);
                prepare(_conversation);
                _beforeLastTurn = _conversation;
            });
            _greetingPending = true;
            _privateAnswer = false;
        }
        greetIfReady();
        return;
    }
    pushDownstream(frame);
    const auto& state = static_cast<const transport::AgentStateFrame&>(*frame);
    _agentState = state.currentState;
    // The flow's reply played to its end, or the user cut it off.
    if (state.transition == transport::AgentStateTransition::SYNTHESIS_COMPLETED) {
        _replyPlaying = false;
    } else if (state.transition == transport::AgentStateTransition::BARGE_IN_INTERRUPT && _replyPlaying) {
        _replyPlaying = false;
        post([this] { _conversation.replyInterrupted(); });
    }
    // A question has finished playing: wait for an answer. Anything else stops waiting.
    if (state.transition == transport::AgentStateTransition::SYNTHESIS_COMPLETED &&
        state.currentState == transport::AgentState::LISTENING && _awaitingAnswer) {
        if (_noInputMs > 0) armNoInput(_noInputMs);
    } else if (state.transition != transport::AgentStateTransition::FOLLOW_UP_WINDOW_CLOSED ||
               state.currentState != transport::AgentState::LISTENING) {
        disarmNoInput();
    }
    // Activated (at start when always on, else by the wake word or button): greet if it's due.
    if (state.previousState == transport::AgentState::IDLE && state.currentState == transport::AgentState::LISTENING)
        greetIfReady();
}

void DialogProcessor::greetIfReady() {
    if (!_greetingPending || !_connected || _agentState == transport::AgentState::IDLE) return;
    _greetingPending = false;
    post([this] { deliver(_conversation.start()); });
}

void DialogProcessor::speak(const Reply& reply, bool filler) {
    if (!filler) {
        _awaitingAnswer = reply.expectsReply;
        _privateAnswer = reply.expectsReply && reply.expectation.secure_entry();
    }
    if (reply.text.empty()) return;
    _replyPlaying = true;
    // How long the mic stays open after this reply: the agent's own setting, and at least until
    // the no-input reprompt of a question.
    auto window = std::make_shared<transport::FollowUpListenFrame>();
    window->listenMs = reply.followUpListenMs;
    if (const int noInputMs = reply.expectsReply ? _noInputMs : 0; noInputMs > 0)
        window->atLeastMs = noInputMs + kNoInputReplyMarginMs;
    if (!filler) pushUpstream(window);
    // A frame per part: a part that can't be interrupted, or has SSML, is spoken on its own.
    const auto& segments = reply.segments;
    for (size_t i = 0; i < segments.size(); ++i) {
        auto text = std::make_shared<transport::TextFrame>();
        text->text = i == 0 ? segments[i].text : " " + segments[i].text;   // listeners join the parts as they come
        text->ssml = segments[i].ssml;
        text->interruptible = segments[i].interruptible;
        text->isFinal = i + 1 == segments.size();
        text->sourceId = getName();
        text->sensitiveValues = reply.sensitiveValues;
        if (text->isFinal) {
            text->isExpecting = reply.expectsReply;
            for (const auto& choice : reply.choices) text->choices.push_back({choice.label, choice.value});
            if (reply.ui.fields_size() > 0) (void)google::protobuf::util::MessageToJsonString(reply.ui, &text->uiJson);
            text->expectBytes = reply.expectation.SerializeAsString();
        }
        pushDownstream(text);
    }
}

} // namespace flow
} // namespace va
