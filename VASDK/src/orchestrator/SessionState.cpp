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
    : _sessionId(std::move(sessionId))
    , _activeFlowId(std::move(activeFlowId))
    , _activeNodeId(std::move(activeNodeId))
    , _createdAtUnixMs(nowUnixMs()) {}

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

void SessionState::clearParameter(const std::string& name) {
    _parameters.mutable_fields()->erase(name);
}

const google::protobuf::Struct& SessionState::parameters() const { return _parameters; }

void SessionState::setActiveFlowId(std::string flowId) { _activeFlowId = std::move(flowId); }
void SessionState::setActiveNodeId(std::string nodeId) { _activeNodeId = std::move(nodeId); }
void SessionState::incrementTurnCount() { ++_turnCount; }
void SessionState::clearAllParameters() { _parameters.clear_fields(); }

} // namespace orchestrator
} // namespace va
