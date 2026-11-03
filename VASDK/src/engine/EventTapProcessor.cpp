#include "engine/EventTapProcessor.h"

#include "vasdk/engine/events.pb.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace va {
namespace engine {

EventDispatcher::EventDispatcher(size_t maxQueued) : _maxQueued(maxQueued), _thread([this] { run(); }) {}

EventDispatcher::~EventDispatcher() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
    }
    _wake.notify_all();
    _thread.join();
}

int EventDispatcher::addListener(EventListener listener) {
    std::lock_guard<std::mutex> lock(_mutex);
    _listeners.emplace_back(_nextId, std::make_shared<EventListener>(std::move(listener)));
    return _nextId++;
}

void EventDispatcher::removeListener(int id) {
    std::lock_guard<std::mutex> lock(_mutex);
    _listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(),
                                    [id](const auto& entry) { return entry.first == id; }),
                     _listeners.end());
}

void EventDispatcher::post(vasdk::engine::Event event) {
    if (event.timestamp_ms() == 0) {
        event.set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.size() >= _maxQueued) {
            _queue.pop_front();
            if (_dropped++ % 100 == 0) spdlog::warn("[EventDispatcher] Listeners too slow: {} events dropped", _dropped);
        }
        _queue.push_back(std::make_shared<vasdk::engine::Event>(std::move(event)));
    }
    _wake.notify_one();
}

void EventDispatcher::run() {
    std::unique_lock<std::mutex> lock(_mutex);
    while (true) {
        _wake.wait(lock, [this] { return _stopping || !_queue.empty(); });
        if (_queue.empty()) return;   // stopping, and everything was delivered
        auto event = std::move(_queue.front());
        _queue.pop_front();
        auto listeners = _listeners;   // a copy: listeners run unlocked and may change the list
        lock.unlock();
        for (const auto& [id, listener] : listeners) {
            try {
                (*listener)(*event);
            } catch (const std::exception& e) {
                spdlog::error("[EventDispatcher] Listener {} threw: {}", id, e.what());
            }
        }
        lock.lock();
    }
}

bool EventTapProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    switch (frame->getType()) {
    case transport::FrameType::TRANSCRIPTION:
    case transport::FrameType::TEXT:
    case transport::FrameType::AGENT_STATE:
    case transport::FrameType::ERROR:
        return true;
    default:
        return false;
    }
}

void EventTapProcessor::processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) {
    vasdk::engine::Event event;
    switch (frame->getType()) {
    case transport::FrameType::TRANSCRIPTION: {
        if (direction != pipeline::FlowDirection::DOWNSTREAM) break;
        auto transcription = std::static_pointer_cast<transport::TranscriptionFrame>(frame);
        auto* transcript = event.mutable_transcript();
        transcript->set_text(transcription->transcriptionResult.text);
        transcript->set_is_final(transcription->isFinal);
        transcript->set_confidence(transcription->transcriptionResult.confidence);
        transcript->set_is_backchannel(transcription->isBackchannel);
        break;
    }
    case transport::FrameType::TEXT: {
        if (direction != pipeline::FlowDirection::DOWNSTREAM) break;
        auto text = std::static_pointer_cast<transport::TextFrame>(frame);
        _reply += text->text;
        if (_reply.empty()) break;   // an empty final closing an empty reply
        auto* reply = event.mutable_agent_reply();
        reply->set_text(_reply);
        reply->set_is_final(text->isFinal);
        if (text->isFinal) {
            reply->set_expects_reply(text->isExpecting);
            for (const auto& choice : text->choices) reply->add_choices(choice.text);
            _reply.clear();
        }
        break;
    }
    case transport::FrameType::AGENT_STATE: {
        if (direction != pipeline::FlowDirection::DOWNSTREAM) break;
        auto agentState = std::static_pointer_cast<transport::AgentStateFrame>(frame);
        // A new or cut-off reply: whatever was streamed before is not part of the next one.
        if (agentState->currentState == transport::AgentState::THINKING ||
            agentState->currentState == transport::AgentState::INTERRUPTED) {
            _reply.clear();
        }
        auto* change = event.mutable_agent_state();
        change->set_state(transport::toString(agentState->currentState));
        change->set_previous_state(transport::toString(agentState->previousState));
        change->set_transition(transport::toString(agentState->transition));
        break;
    }
    case transport::FrameType::ERROR: {
        auto errorFrame = std::static_pointer_cast<transport::ErrorFrame>(frame);
        auto* error = event.mutable_error();
        error->set_severity(errorFrame->severity == transport::ErrorSeverity::WARNING ? vasdk::engine::Error::SEVERITY_WARNING
                            : errorFrame->severity == transport::ErrorSeverity::FATAL ? vasdk::engine::Error::SEVERITY_FATAL
                                                                                      : vasdk::engine::Error::SEVERITY_ERROR);
        error->set_source(errorFrame->sourceId);
        error->set_message(errorFrame->errorMessage);
        error->set_code(errorFrame->errorCode);
        break;
    }
    default:
        break;
    }
    if (event.kind_case() != vasdk::engine::Event::KIND_NOT_SET) _dispatcher->post(std::move(event));

    if (direction == pipeline::FlowDirection::DOWNSTREAM) pushDownstream(frame);
    else pushUpstream(frame);
}

} // namespace engine
} // namespace va
