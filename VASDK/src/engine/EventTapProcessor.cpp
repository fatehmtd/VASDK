#include "engine/EventTapProcessor.h"
#include "flow/Tools.h"

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
    if (event.session_id().empty() && !_sessionId.empty()) event.set_session_id(_sessionId);
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

void EventTapProcessor::postListeningWindow(const transport::AgentStateFrame& frame) {
    vasdk::engine::Event event;
    auto* window = event.mutable_listening_window();
    if (frame.listenWindowMs != 0) {
        window->set_state(vasdk::engine::ListeningWindow::OPENED);
        window->set_closes_in_ms(frame.listenWindowMs);
        _listeningWindowOpen = true;
    } else if (!_listeningWindowOpen) {
        return;
    } else if (frame.transition == transport::AgentStateTransition::USER_STARTED_SPEAKING) {
        window->set_state(vasdk::engine::ListeningWindow::EXTENDED);
        window->set_reason("user_speaking");
        _listeningWindowOpen = false;
    } else {
        window->set_state(vasdk::engine::ListeningWindow::CLOSED);
        window->set_reason(frame.transition == transport::AgentStateTransition::FOLLOW_UP_WINDOW_CLOSED
                               ? "timeout" : transport::toString(frame.transition));
        _listeningWindowOpen = false;
    }
    _dispatcher->post(std::move(event));
}

bool EventTapProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    switch (frame->getType()) {
    case transport::FrameType::TRANSCRIPTION:
    case transport::FrameType::TEXT:
    case transport::FrameType::AGENT_STATE:
    case transport::FrameType::ERROR:
    case transport::FrameType::VAD_STATE:
    case transport::FrameType::SPEAKER_IDENTIFICATION:
    case transport::FrameType::SPEAKER_ENROLLMENT:
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
        const auto& said = transcription->transcriptionResult.text;
        transcript->set_text(transcription->sensitive ? flow::maskValue(said) : said);
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
        reply->set_text(flow::redact(_reply, text->sensitiveValues));
        reply->set_is_final(text->isFinal);
        if (text->isFinal) {
            reply->set_expects_reply(text->isExpecting);
            for (const auto& choice : text->choices) {
                reply->add_choices(choice.text);
                reply->add_choice_values(choice.value);
            }
            reply->set_ui_json(text->uiJson);
            if (!text->expectBytes.empty() && !reply->mutable_expect()->ParseFromString(text->expectBytes))
                spdlog::warn("[EventTap] unreadable expectation on a reply");
            if (_saidObserver) _saidObserver(reply->text(), text->sourceId);
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
        _dispatcher->post(std::move(event));
        event.Clear();
        postListeningWindow(*agentState);
        break;
    }
    case transport::FrameType::VAD_STATE: {
        if (direction != pipeline::FlowDirection::DOWNSTREAM) break;
        using State = vasdk::engine::UserSpeech;
        switch (std::static_pointer_cast<transport::VADStateFrame>(frame)->state) {
        case transport::VADState::STARTING: event.mutable_user_speech()->set_state(State::STARTED); break;
        case transport::VADState::ONGOING:
        case transport::VADState::RESUMING: event.mutable_user_speech()->set_state(State::ONGOING); break;
        case transport::VADState::ENDED: event.mutable_user_speech()->set_state(State::ENDED); break;
        default: break;
        }
        break;
    }
    case transport::FrameType::SPEAKER_IDENTIFICATION: {
        if (direction != pipeline::FlowDirection::DOWNSTREAM) break;
        auto speaker = std::static_pointer_cast<transport::SpeakerIdentificationFrame>(frame);
        auto* identified = event.mutable_speaker_identified();
        identified->set_speaker_id(speaker->speakerId);
        identified->set_similarity(speaker->confidence);
        identified->set_new_speaker(speaker->isNewSpeaker);
        identified->set_tracked_similarity(speaker->trackedSimilarity);
        break;
    }
    case transport::FrameType::SPEAKER_ENROLLMENT: {
        auto enrollment = std::static_pointer_cast<transport::SpeakerEnrollmentFrame>(frame);
        if (direction != pipeline::FlowDirection::DOWNSTREAM || enrollment->status == transport::SpeakerEnrollmentStatus::REQUESTED) break;
        auto* outcome = event.mutable_speaker_enrollment();
        outcome->set_speaker_id(enrollment->speakerId);
        outcome->set_status(enrollment->status == transport::SpeakerEnrollmentStatus::ENROLLED
                                ? vasdk::engine::SpeakerEnrollment::ENROLLED : vasdk::engine::SpeakerEnrollment::FAILED);
        outcome->set_error(enrollment->errorMessage);
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
