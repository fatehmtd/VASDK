#include <VASDK/SessionManager.h>

#include <spdlog/spdlog.h>

namespace va {

SessionManager::SessionManager(std::shared_ptr<engine::Runtime> runtime) : _runtime(std::move(runtime)) {}

SessionManager::~SessionManager() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [id, session] : _sessions) session->stop();
}

std::shared_ptr<engine::Session> SessionManager::createSession(engine::Session::Options options, std::string* error) {
    const std::string id = options.id;
    auto fail = [&](const std::string& message) -> std::shared_ptr<engine::Session> {
        spdlog::warn("[SessionManager] Session '{}' not created: {}", id, message);
        if (error) *error = message;
        return nullptr;
    };
    if (id.empty()) return fail("Options::id is required");
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_sessions.count(id)) return fail("a session with this id is already running");
    }
    std::string reason;
    std::shared_ptr<engine::Session> session = _runtime->createSession(std::move(options), &reason);
    if (!session) return fail(reason);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_sessions.emplace(id, session).second) return fail("a session with this id is already running");
    spdlog::info("[SessionManager] Session '{}' created ({} active)", id, _sessions.size());
    return session;
}

std::shared_ptr<engine::Session> SessionManager::session(const std::string& id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto found = _sessions.find(id);
    return found == _sessions.end() ? nullptr : found->second;
}

void SessionManager::destroySession(const std::string& id) {
    std::shared_ptr<engine::Session> session;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto found = _sessions.find(id);
        if (found == _sessions.end()) return;
        session = std::move(found->second);
        _sessions.erase(found);
    }
    session->stop();
    spdlog::info("[SessionManager] Session '{}' ended", id);
}

size_t SessionManager::activeSessionCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sessions.size();
}

} // namespace va
