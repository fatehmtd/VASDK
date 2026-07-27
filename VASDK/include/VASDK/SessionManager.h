#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalStatePersistence.h>
#include <VASDK/goal/IntentRouter.h>
#include <VASDK/goal/GoalManager.h>
#include <VASDK/metrics/CallMetrics.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/VADProvider.h>
#include <VASDK/Pipeline.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace va {

//  Resource pool 

/// Thread-safe pool of pre-initialised model instances.
/// Callers acquire() a resource (blocking with timeout), use it, then release() it.
///
/// The pool state lives in a shared_ptr so ResourcePool itself is a copyable/movable
/// handle - this allows passing it by value into SessionManager's constructor.
template<typename T>
class ResourcePool {
public:
    explicit ResourcePool(std::vector<std::shared_ptr<T>> resources)
        : _state(std::make_shared<State>(std::move(resources))) {}

    /// Acquire a resource; blocks up to `timeout`. Returns nullptr on timeout.
    std::shared_ptr<T> acquire(std::chrono::milliseconds timeout
                                    = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lock(_state->mutex);
        if (!_state->cv.wait_for(lock, timeout, [this]{ return !_state->items.empty(); })) {
            return nullptr;
        }
        auto r = _state->items.back();
        _state->items.pop_back();
        return r;
    }

    void release(std::shared_ptr<T> resource) {
        {
            std::lock_guard<std::mutex> lock(_state->mutex);
            _state->items.push_back(std::move(resource));
        }
        _state->cv.notify_one();
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(_state->mutex);
        return _state->items.size();
    }

private:
    struct State {
        explicit State(std::vector<std::shared_ptr<T>> v) : items(std::move(v)) {}
        mutable std::mutex         mutex;
        std::condition_variable    cv;
        std::vector<std::shared_ptr<T>> items;
    };
    std::shared_ptr<State> _state;
};

//  Call metadata 

struct CallMetadata {
    std::string callId;
    std::string callerANI;  ///< Caller phone number (used as persistence key)
    std::string calledDNIS; ///< Dialled number (determines which goal set to load)
};

//  Call session 

struct CallSession {
    std::string callId;
    std::chrono::steady_clock::time_point startTime;

    std::shared_ptr<va::audio::GenericTranscriptionProvider>  stt;
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> tts;

    std::shared_ptr<va::goal::IntentRouter>      intentRouter;
    std::shared_ptr<va::goal::GoalManager>       goalManager;
    std::shared_ptr<va::pipeline::Pipeline>      pipeline;
    std::shared_ptr<va::metrics::CallMetrics>    metrics;
};

//  Session manager config 

struct SessionManagerConfig {
    int maxConcurrentSessions = 10;
    std::chrono::minutes maxCallDuration{30};

    std::shared_ptr<va::goal::GoalStatePersistence> persistence;

    /// Maps a DNIS to the set of intent options available for that number.
    /// Different dialled numbers can offer different goal sets.
    std::function<std::vector<va::goal::IntentOption>(const std::string& dnis)> goalSetForDNIS;

    /// Factory: build a fully-wired Pipeline for a session.
    /// Receives the session's dedicated STT and TTS instances from the pool.
    std::function<std::shared_ptr<va::pipeline::Pipeline>(
        const CallMetadata&,
        std::shared_ptr<va::audio::GenericTranscriptionProvider>,
        std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>)> pipelineFactory;
};

//  Session manager 

class VASDK_EXPORT SessionManager {
public:
    SessionManager(
        SessionManagerConfig config,
        ResourcePool<va::audio::GenericTranscriptionProvider> sttPool,
        ResourcePool<va::audio::GenericVoiceSynthesisProvider> ttsPool,
        std::shared_ptr<va::audio::VADProvider> sharedVAD);

    /// Called when a new call arrives. Acquires STT/TTS from pool, builds pipeline.
    /// Returns nullptr if the pool is exhausted or max sessions reached.
    std::shared_ptr<CallSession> createSession(const CallMetadata& meta);

    /// Called when a call ends. Releases STT/TTS back to pool and stops the pipeline.
    void destroySession(const std::string& callId);

    size_t activeSessionCount() const;

private:
    SessionManagerConfig _config;
    ResourcePool<va::audio::GenericTranscriptionProvider>  _sttPool;
    ResourcePool<va::audio::GenericVoiceSynthesisProvider> _ttsPool;
    std::shared_ptr<va::audio::VADProvider>                _sharedVAD;

    std::unordered_map<std::string, std::shared_ptr<CallSession>> _sessions;
    mutable std::mutex _mutex;
};

} // namespace va
