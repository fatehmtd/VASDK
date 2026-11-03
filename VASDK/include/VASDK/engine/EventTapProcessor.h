#pragma once

#include "vasdk_export.h"
#include "../FrameProcessor.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vasdk::engine { class Event; }

namespace va {
namespace engine {

using EventListener = std::function<void(const vasdk::engine::Event&)>;

/**
 * @brief Delivers events to listeners on one thread of its own, in order.
 *
 * post() never blocks the caller (a pipeline or tool thread): when listeners fall behind,
 * the oldest queued events are dropped and a warning is logged.
 */
class VASDK_EXPORT EventDispatcher {
public:
    explicit EventDispatcher(size_t maxQueued = 1024);
    ~EventDispatcher();   // delivers what is queued, then stops
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;

    /** Returns an id for removeListener(). Listeners may add or remove listeners themselves. */
    int addListener(EventListener listener);
    void removeListener(int id);
    /** Thread-safe. Stamps timestamp_ms and session_id when they are unset. */
    void post(vasdk::engine::Event event);
    /** Set before the first post(). */
    void setSessionId(std::string id) { _sessionId = std::move(id); }

private:
    void run();

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<std::shared_ptr<vasdk::engine::Event>> _queue;
    std::vector<std::pair<int, std::shared_ptr<EventListener>>> _listeners;
    size_t _maxQueued;
    std::string _sessionId;
    int _nextId = 1;
    size_t _dropped = 0;
    bool _stopping = false;
    std::thread _thread;
};

/**
 * @brief Turns the frames that pass it into events: transcripts, agent replies, agent states
 *        and errors. Frames pass through unchanged.
 *
 * Place it where all of those still flow downstream: VAEngine puts it just before TTS.
 */
class VASDK_EXPORT EventTapProcessor : public pipeline::FrameProcessor {
public:
    explicit EventTapProcessor(std::shared_ptr<EventDispatcher> dispatcher) : _dispatcher(std::move(dispatcher)) {}
    std::string getName() const override { return "EventTap"; }
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override;

private:
    std::shared_ptr<EventDispatcher> _dispatcher;
    std::string _reply;   // the agent reply streamed so far
};

} // namespace engine
} // namespace va
