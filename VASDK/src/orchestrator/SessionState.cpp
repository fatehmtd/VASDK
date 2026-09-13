#include <VASDK/orchestrator/SessionState.h>

#include <chrono>

namespace va {
namespace orchestrator {

namespace {
int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

SessionState::SessionState(std::string sessionId, std::string activeFlowId, std::string activeNodeId)
    : SessionState(std::move(sessionId), std::move(activeFlowId), std::move(activeNodeId), nowUnixMs()) {}

SessionState::SessionState(std::string sessionId, std::string activeFlowId, std::string activeNodeId,
                            int64_t createdAtUnixMs)
    : _sessionId(std::move(sessionId))
    , _activeFlowId(std::move(activeFlowId))
    , _activeNodeId(std::move(activeNodeId))
    , _createdAtUnixMs(createdAtUnixMs) {}

const std::string& SessionState::sessionId() const { return _sessionId; }
const std::string& SessionState::activeFlowId() const { return _activeFlowId; }
const std::string& SessionState::activeNodeId() const { return _activeNodeId; }
int SessionState::turnCount() const { return _turnCount; }
int64_t SessionState::createdAtUnixMs() const { return _createdAtUnixMs; }

std::optional<google::protobuf::Value> SessionState::getParameter(const std::string& name) const {
    auto it = _parameters.fields().find(name);
    if (it == _parameters.fields().end()) return std::nullopt;
    return it->second;
}

void SessionState::setParameter(const std::string& name, google::protobuf::Value value) {
    (*_parameters.mutable_fields())[name] = std::move(value);
}

void SessionState::setParameter(const std::string& name, google::protobuf::Value value, std::string source) {
    setParameter(name, std::move(value));
    ParameterMeta& meta = _parameterMeta[name];   // creates {attempts:0} if this parameter's
                                                    // ask cycle never called noteParameterAskStarted/
                                                    // incrementParameterAttempts (e.g. it validated
                                                    // on the very first try) - attempts left as-is
                                                    // otherwise, a historical failed-attempt count.
    meta.set_source(std::move(source));
    meta.set_last_updated_unix_ms(nowUnixMs());
}

void SessionState::clearParameter(const std::string& name) {
    _parameters.mutable_fields()->erase(name);
    _parameterMeta.erase(name);   // a future re-ask of this same name starts its attempts over.
}

const google::protobuf::Struct& SessionState::parameters() const { return _parameters; }

void SessionState::setActiveFlowId(std::string flowId) { _activeFlowId = std::move(flowId); }
void SessionState::setActiveNodeId(std::string nodeId) { _activeNodeId = std::move(nodeId); }
void SessionState::incrementTurnCount() { ++_turnCount; }

void SessionState::clearAllParameters() {
    _parameters.clear_fields();
    _parameterMeta.clear();
}

void SessionState::noteParameterAskStarted(const std::string& name) {
    _parameterMeta.try_emplace(name);
}

int SessionState::incrementParameterAttempts(const std::string& name) {
    ParameterMeta& meta = _parameterMeta[name];
    meta.set_attempts(meta.attempts() + 1);
    return meta.attempts();
}

int SessionState::parameterAttempts(const std::string& name) const {
    auto it = _parameterMeta.find(name);
    return it == _parameterMeta.end() ? 0 : it->second.attempts();
}

void SessionState::setParameterMeta(const std::string& name, ParameterMeta meta) {
    _parameterMeta[name] = std::move(meta);
}

const std::unordered_map<std::string, ParameterMeta>& SessionState::parameterMetaAll() const {
    return _parameterMeta;
}

bool SessionState::hasPendingAsk() const { return _hasPendingAsk; }
const PendingAsk& SessionState::pendingAsk() const { return _pendingAsk; }

void SessionState::setPendingAsk(PendingAsk ask) {
    _pendingAsk = std::move(ask);
    _hasPendingAsk = true;
}

void SessionState::clearPendingAsk() {
    _pendingAsk.Clear();
    _hasPendingAsk = false;
}

int SessionState::incrementPendingAskAttempts() {
    _pendingAsk.set_attempts(_pendingAsk.attempts() + 1);
    return _pendingAsk.attempts();
}

} // namespace orchestrator
} // namespace va
