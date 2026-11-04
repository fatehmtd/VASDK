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

// restart() from an app thread, handled on the processor's own thread.
class RestartFrame : public transport::Frame {
public:
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<RestartFrame>(); }
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

// The inactivity timer fired, handled on the processor's own thread.
class InactivityFrame : public transport::Frame {
public:
    explicit InactivityFrame(uint64_t generation) : generation(generation) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<InactivityFrame>(generation); }
    const uint64_t generation;
};

// A second has passed: time to check schedule and context_change triggers.
class TickFrame : public transport::Frame {
public:
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<TickFrame>(); }
};

// fireTrigger() from an app thread, decided on the processor's own.
class TriggerFrame : public transport::Frame {
public:
    TriggerFrame(std::string name, google::protobuf::Struct payload) : name(std::move(name)), payload(std::move(payload)) {}
    transport::FrameType getType() const override { return transport::FrameType::CONTROL; }
    std::shared_ptr<transport::Frame> clone() const override { return std::make_shared<TriggerFrame>(name, payload); }
    const std::string name;
    const google::protobuf::Struct payload;
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
    bool fromTrigger = false;                  // a trigger's; with nothing to say, it changes nothing
};
} // namespace

DialogProcessor::DialogProcessor(std::shared_ptr<const CompiledAgent> agent)
    : _conversation(agent), _beforeLastTurn(agent), _agent(agent) {
    prepare(_conversation);
    _inactivityTriggers = _conversation.inactivityTriggers();
    std::sort(_inactivityTriggers.begin(), _inactivityTriggers.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& trigger : agent->definition().triggers())
        _timedTriggers = _timedTriggers || trigger.source().has_schedule() || trigger.source().has_context_change();
}

void DialogProcessor::prepare(FlowRuntime& conversation) {
    conversation.setContextSource(_contextSource);
    if (_toolCaller) conversation.setToolCaller(_toolCaller);
    if (_nlu) conversation.setNlu(_nlu);
    if (_embeddings) conversation.setEmbeddings(_embeddings);
    if (_llm) conversation.setLlm(_llm);
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
           dynamic_cast<const ReplyFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const RestartFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const InactivityFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const TriggerFrame*>(frame.get()) != nullptr ||
           dynamic_cast<const TickFrame*>(frame.get()) != nullptr;
}

bool DialogProcessor::initialize() {
    if (!FrameProcessor::initialize()) return false;
    if (getMetricsConfig().enabled && _collector) {
        auto counter = [this](metrics::CounterHandle& handle, const char* name, const char* help) {
            handle.initialize(_collector, metrics::MetricDescriptor{name, metrics::MetricType::Counter, help});
        };
        counter(_flowMetrics.turns, "flow_turns", "User turns the agent answered");
        counter(_flowMetrics.noMatch, "flow_no_match", "Turns no route understood");
        counter(_flowMetrics.noInput, "flow_no_input", "Questions met with silence");
        counter(_flowMetrics.maxReprompts, "flow_max_reprompts", "Too many misses in a row");
        counter(_flowMetrics.toolErrors, "flow_tool_errors", "call_tool failures");
        counter(_flowMetrics.clarify, "flow_clarify", "Did-you-mean questions");
        counter(_flowMetrics.pageChanges, "flow_page_changes", "Pages entered");
        counter(_flowMetrics.sessionsEnded, "flow_sessions_ended", "Conversations the agent ended");
        counter(_flowMetrics.passedToLlm, "flow_passed_to_llm", "Turns passed on to the LLM");
        counter(_flowMetrics.triggersSaid, "flow_triggers_said", "Triggers delivered");
        _flowMetrics.turnMs.initialize(_collector, metrics::MetricDescriptor{"flow_turn_ms", metrics::MetricType::Sampler,
                                                                            "Time to answer a user turn"});
    }
    return true;
}

void DialogProcessor::count(const Reply& reply) {
    if (reply.event == kEventNoMatch) _flowMetrics.noMatch.increment();
    else if (reply.event == kEventNoInput) _flowMetrics.noInput.increment();
    else if (reply.event == kEventMaxReprompts) _flowMetrics.maxReprompts.increment();
    else if (reply.event == kEventToolError) _flowMetrics.toolErrors.increment();
    else if (reply.event == kEventClarify) _flowMetrics.clarify.increment();
    for (size_t i = 0; i < reply.pageChanges.size(); ++i) _flowMetrics.pageChanges.increment();
    for (const auto& trigger : reply.triggers)
        if (trigger.decision == vasdk::flow::STARTED) _flowMetrics.triggersSaid.increment();
    if (reply.endsSession) _flowMetrics.sessionsEnded.increment();
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
    if (_timedTriggers) arm(_tick, 1000);
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

void DialogProcessor::deliver(Reply reply, std::shared_ptr<transport::Frame> turn, bool mayPassOn, bool fromTrigger) {
    count(reply);
    if (_observer) _observer(reply, _conversation);
    auto frame = std::make_shared<ReplyFrame>();
    frame->reply = std::move(reply);
    frame->noInputMs = _conversation.noInputMs();
    frame->turn = std::move(turn);
    frame->mayPassOn = mayPassOn;
    frame->fromTrigger = fromTrigger;
    enqueueFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
}

void DialogProcessor::arm(Timer& timer, int ms) {
    std::lock_guard<std::mutex> lock(_timerMutex);
    ++timer.generation;
    timer.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    _timerChanged.notify_all();
}

void DialogProcessor::disarm(Timer& timer) {
    std::lock_guard<std::mutex> lock(_timerMutex);
    if (!timer.deadline) return;
    ++timer.generation;
    timer.deadline.reset();
    _timerChanged.notify_all();
}

// After a reply has played: the next inactivity trigger, counted from now.
void DialogProcessor::armInactivity() {
    if (_nextInactivity < _inactivityTriggers.size()) arm(_inactivity, _inactivityTriggers[_nextInactivity].second);
}

void DialogProcessor::noInputTimerLoop() {
    std::unique_lock<std::mutex> lock(_timerMutex);
    while (!_timerStop) {
        const Timer* next = nullptr;
        for (const Timer* timer : {&_noInput, &_inactivity, &_tick})
            if (timer->deadline && (!next || *timer->deadline < *next->deadline)) next = timer;
        if (!next) {
            _timerChanged.wait(lock);
            continue;
        }
        const auto deadline = *next->deadline;
        const auto generation = next->generation;
        if (_timerChanged.wait_until(lock, deadline) != std::cv_status::timeout || generation != next->generation) continue;
        const bool noInput = next == &_noInput, tick = next == &_tick;
        (noInput ? _noInput : tick ? _tick : _inactivity).deadline.reset();
        lock.unlock();
        if (noInput) enqueueFrame(std::make_shared<NoInputFrame>(generation), pipeline::FlowDirection::DOWNSTREAM);
        else if (tick) enqueueFrame(std::make_shared<TickFrame>(), pipeline::FlowDirection::DOWNSTREAM);
        else enqueueFrame(std::make_shared<InactivityFrame>(generation), pipeline::FlowDirection::DOWNSTREAM);
        lock.lock();
    }
}

void DialogProcessor::fireTrigger(const std::string& name, const google::protobuf::Struct& payload) {
    enqueueFrame(std::make_shared<TriggerFrame>(name, payload), pipeline::FlowDirection::DOWNSTREAM);
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

void DialogProcessor::setNlu(std::shared_ptr<const nlu::NluProvider> provider,
                             std::shared_ptr<const nlu::EmbeddingNluProvider> embeddings) {
    _nlu = std::move(provider);
    _embeddings = std::move(embeddings);
    prepare(_conversation);
    prepare(_beforeLastTurn);
}

void DialogProcessor::setLlm(std::shared_ptr<const llm::Completion> completion) {
    _llm = std::move(completion);
    prepare(_conversation);
    prepare(_beforeLastTurn);
}

void DialogProcessor::setToolCaller(ToolCaller caller) {
    _toolCaller = std::move(caller);
    _conversation.setToolCaller(_toolCaller);
    _beforeLastTurn.setToolCaller(_toolCaller);
}

void DialogProcessor::restart() {
    enqueueFrame(std::make_shared<RestartFrame>(), pipeline::FlowDirection::DOWNSTREAM);
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
        // A trigger queued or dropped: the conversation is where it was.
        if (ready->fromTrigger && ready->reply.text.empty() && !ready->reply.expectsReply && !ready->reply.endsSession) return;
        _noInputMs = ready->noInputMs;
        if (ready->mayPassOn && _passUnmatchedTurns) {
            _flowMetrics.passedToLlm.increment();
            pushDownstream(ready->turn);
            return;
        }
        if (ready->reply.endsSession) _greetingPending = true;   // on the next activation or caller
        speak(ready->reply);
        return;
    }
    if (frame->getType() == transport::FrameType::USER_TURN) {
        disarmNoInput();
        disarm(_inactivity);
        _nextInactivity = 0;
        _greetingPending = false;   // the user spoke first: their words are routed on the start page
        const bool privateAnswer = _privateAnswer;
        post([this, frame, privateAnswer] {
            const auto& turn = static_cast<const transport::UserTurnFrame&>(*frame);
            if (turn.replacesPrevious) _conversation = _beforeLastTurn;
            _beforeLastTurn = _conversation;
            const auto started = std::chrono::steady_clock::now();
            _conversation.setLlmHasConversation(_llmTookLastTurn && _passUnmatchedTurns);
            auto reply = _conversation.handleText(turn.text);
            _flowMetrics.turns.increment();
            _flowMetrics.turnMs.observe(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            spdlog::info("[Dialog] \"{}\" → {} on {}/{}", privateAnswer ? maskValue(turn.text) : redact(turn.text, reply.sensitiveValues),
                         reply.matched ? "route " + reply.intent : "not understood", _conversation.flowName(), _conversation.pageName());
            const bool mayPassOn = !reply.matched && !privateAnswer;   // a private answer never goes to an LLM
            // The LLM takes the turn: the agent's miss (its count, a no-match or max-reprompts handler) never happened.
            const bool passOn = mayPassOn && _passUnmatchedTurns;
            if (passOn) _conversation = _beforeLastTurn;
            // The LLM keeps the conversation until the agent takes a route ("You're welcome." doesn't).
            if (passOn || !reply.intent.empty()) _llmTookLastTurn = passOn;
            deliver(std::move(reply), frame, mayPassOn);
        });
        return;
    }
    if (auto noInput = dynamic_cast<const NoInputFrame*>(frame.get())) {
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            if (noInput->generation != _noInput.generation) return;   // moved on since it fired
        }
        post([this] {
            _beforeLastTurn = _conversation;
            _conversation.setLlmHasConversation(_llmTookLastTurn && _passUnmatchedTurns);
            auto reply = _conversation.handleNoInput();
            spdlog::info("[Dialog] no input → {} on {}/{}", reply.event, _conversation.flowName(), _conversation.pageName());
            deliver(std::move(reply));
        });
        return;
    }
    if (auto inactivity = dynamic_cast<const InactivityFrame*>(frame.get())) {
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            if (inactivity->generation != _inactivity.generation) return;
        }
        if (_nextInactivity >= _inactivityTriggers.size()) return;
        const std::string name = _inactivityTriggers[_nextInactivity++].first;
        post([this, name] { deliver(_conversation.fireTrigger(name), nullptr, false, true); });
        return;
    }
    if (dynamic_cast<const TickFrame*>(frame.get())) {
        arm(_tick, 1000);
        const bool betweenTurns = !_replyPlaying && !_userSpeaking && _agentState != transport::AgentState::THINKING &&
                                  _agentState != transport::AgentState::SPEAKING;
        post([this, betweenTurns] {
            auto reply = _conversation.checkTriggers(betweenTurns);
            if (!reply.triggers.empty()) deliver(std::move(reply), nullptr, false, true);
        });
        return;
    }
    if (auto trigger = std::dynamic_pointer_cast<TriggerFrame>(frame)) {
        // Between turns: no reply playing or on its way, and the user isn't speaking.
        const bool betweenTurns = !_replyPlaying && !_userSpeaking && _agentState != transport::AgentState::THINKING &&
                                  _agentState != transport::AgentState::SPEAKING;
        post([this, trigger, betweenTurns] {
            deliver(_conversation.fireTrigger(trigger->name, trigger->payload, betweenTurns), nullptr, false, true);
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
    auto startOver = [this] {
        disarmNoInput();
        post([this] {
            _conversation = FlowRuntime(_agent);
            prepare(_conversation);
            _beforeLastTurn = _conversation;
        });
        _greetingPending = true;
        _privateAnswer = false;
        _awaitingAnswer = false;
    };
    if (dynamic_cast<const RestartFrame*>(frame.get())) {
        startOver();
        return;
    }
    if (auto connection = dynamic_cast<const ConnectionFrame*>(frame.get())) {
        if (connection->connected == _connected) return;
        _connected = connection->connected;
        if (!_connected) {
            // The caller left: whoever connects next starts a new conversation.
            spdlog::info("[Dialog] Disconnected - the next conversation starts over");
            startOver();
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
    if (state.transition == transport::AgentStateTransition::USER_STARTED_SPEAKING) _userSpeaking = true;
    else if (state.currentState != transport::AgentState::LISTENING) _userSpeaking = false;
    if (state.transition == transport::AgentStateTransition::SYNTHESIS_COMPLETED && !_greetingPending) {
        armInactivity();
        post([this] {   // a trigger that waited for this reply to end
            auto reply = _conversation.deliverQueued();
            if (!reply.triggers.empty()) deliver(std::move(reply), nullptr, false, true);
        });
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
    if (!filler) {   // how to listen for the answer; a reply that asks nothing resets it
        auto policy = std::make_shared<transport::TurnPolicyFrame>();
        using TurnEnd = transport::TurnPolicyFrame::TurnEnd;
        const auto turnEnd = reply.expectsReply ? reply.expectation.turn_end() : vasdk::flow::Expectation::NORMAL;
        policy->turnEnd = turnEnd == vasdk::flow::Expectation::PATIENT ? TurnEnd::Patient
                        : turnEnd == vasdk::flow::Expectation::SNAPPY  ? TurnEnd::Snappy
                                                                        : TurnEnd::Normal;
        if (reply.expectsReply) policy->hints.assign(reply.expectation.hints().begin(), reply.expectation.hints().end());
        pushUpstream(policy);
    }
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
