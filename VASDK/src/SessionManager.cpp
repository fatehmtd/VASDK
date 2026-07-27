#include <VASDK/SessionManager.h>
#include <spdlog/spdlog.h>

namespace va {

SessionManager::SessionManager(
    SessionManagerConfig config,
    ResourcePool<va::audio::GenericTranscriptionProvider>  sttPool,
    ResourcePool<va::audio::GenericVoiceSynthesisProvider> ttsPool,
    std::shared_ptr<va::audio::VADProvider> sharedVAD)
    : _config(std::move(config))
    , _sttPool(std::move(sttPool))
    , _ttsPool(std::move(ttsPool))
    , _sharedVAD(std::move(sharedVAD)) {}

std::shared_ptr<CallSession> SessionManager::createSession(const CallMetadata& meta) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (static_cast<int>(_sessions.size()) >= _config.maxConcurrentSessions) {
            spdlog::warn("[SessionManager] Max concurrent sessions ({}) reached - rejecting call {}",
                         _config.maxConcurrentSessions, meta.callId);
            return nullptr;
        }
    }

    auto stt = _sttPool.acquire();
    if (!stt) {
        spdlog::warn("[SessionManager] STT pool exhausted - rejecting call {}", meta.callId);
        return nullptr;
    }
    auto tts = _ttsPool.acquire();
    if (!tts) {
        _sttPool.release(stt);
        spdlog::warn("[SessionManager] TTS pool exhausted - rejecting call {}", meta.callId);
        return nullptr;
    }

    auto session        = std::make_shared<CallSession>();
    session->callId     = meta.callId;
    session->startTime  = std::chrono::steady_clock::now();
    session->stt        = stt;
    session->tts        = tts;

    auto callMetrics        = std::make_shared<va::metrics::CallMetrics>();
    callMetrics->callId     = meta.callId;
    callMetrics->callerANI  = meta.callerANI;
    callMetrics->calledDNIS = meta.calledDNIS;
    callMetrics->callStart  = session->startTime;
    session->metrics        = callMetrics;

    // Restore partial goal state if available
    std::optional<nlohmann::json> savedState;
    if (_config.persistence && !meta.callerANI.empty()) {
        savedState = _config.persistence->load(meta.callerANI);
    }

    // Build pipeline using the app-provided factory
    if (_config.pipelineFactory) {
        session->pipeline = _config.pipelineFactory(meta, stt, tts);
    }

    spdlog::info("[SessionManager] Session created: callId={} ANI={} DNIS={}",
                 meta.callId, meta.callerANI, meta.calledDNIS);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _sessions[meta.callId] = session;
    }
    return session;
}

void SessionManager::destroySession(const std::string& callId) {
    std::shared_ptr<CallSession> session;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _sessions.find(callId);
        if (it == _sessions.end()) return;
        session = it->second;
        _sessions.erase(it);
    }

    if (session->pipeline) {
        session->pipeline->stop();
    }

    if (session->metrics) {
        session->metrics->callEnd = std::chrono::steady_clock::now();
        spdlog::info("[SessionManager] Call {} - duration: {:.1f}s completed={} escalated={}",
                     callId,
                     session->metrics->handleTime().count(),
                     session->metrics->goalCompleted,
                     session->metrics->escalated);
    }

    if (session->stt) _sttPool.release(session->stt);
    if (session->tts) _ttsPool.release(session->tts);

    spdlog::info("[SessionManager] Session destroyed: callId={}", callId);
}

size_t SessionManager::activeSessionCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sessions.size();
}

} // namespace va
