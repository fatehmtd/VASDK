#include <VASDK/orchestrator/OrchestratorConversationSimulator.h>

#include <algorithm>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace va {
namespace orchestrator {

OrchestratorConversationSimulator::OrchestratorConversationSimulator(va::orchestrator::Agent agent)
    : _agent(std::move(agent)) {}

OrchestratorConversationSimulator::OrchestratorConversationSimulator(
    va::orchestrator::Agent agent, const va::orchestrator::SessionSnapshot& snapshot)
    : _agent(std::move(agent))
    , _restoreFrom(snapshot) {}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::addTurn(SimTurn turn) {
    _turns.push_back(std::move(turn));
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::addTurn(std::string userText) {
    _turns.push_back(SimTurn{std::move(userText)});
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::setInjectedNLUResult(
    const std::string& utterance, va::orchestrator::NLUResult result) {
    _injectedNluResults[utterance] = std::move(result);
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::setNLUProvider(
    std::shared_ptr<va::orchestrator::NLUProvider> provider) {
    _nluProvider = std::move(provider);
    return *this;
}

SimulationResult OrchestratorConversationSimulator::run() {
    _ran = false;
    _result = {};

    _engine = _restoreFrom
                  ? std::make_shared<OrchestratorEngine>(_agent, *_restoreFrom)
                  : std::make_shared<OrchestratorEngine>(_agent);

    _engine->setOnSay([this](const std::string& text) {
        _result.sayOutputs.push_back(text);
    });
    _engine->setOnFlowComplete([this](const std::string& flowId, const SessionState&) {
        _result.sessionEnded = true;
        _result.completedFlowId = flowId;
    });
    _engine->setNLUProvider(_nluProvider); // fine to pass null - see OrchestratorEngine::setNLUProvider

    _engine->start(); // no-op if this instance was constructed from a snapshot

    for (size_t i = 0; i < _turns.size(); ++i) {
        if (_engine->sessionEnded()) break;
        spdlog::debug("[OrchestratorConversationSimulator] Turn {}/{}: \"{}\"",
                      i + 1, _turns.size(), _turns[i].userText);
        auto injected = _injectedNluResults.find(_turns[i].userText);
        const va::orchestrator::NLUResult* injectedPtr =
            injected != _injectedNluResults.end() ? &injected->second : nullptr;
        _engine->handleUtterance(_turns[i].userText, injectedPtr);
    }

    _ran = true;
    return _result;
}

va::orchestrator::SessionSnapshot OrchestratorConversationSimulator::snapshot() const {
    requireRan("snapshot()");
    return _engine->snapshot();
}

void OrchestratorConversationSimulator::requireRan(const char* what) const {
    if (!_ran) {
        throw std::logic_error(std::string("OrchestratorConversationSimulator: call run() before ") + what);
    }
}

//  Fluent assertions

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertActiveNode(const std::string& nodeId) {
    requireRan("assertActiveNode()");
    const auto& actual = _engine->sessionState().activeNodeId();
    if (actual != nodeId) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertActiveNode failed: expected '"
                                  + nodeId + "' got '" + actual + "'");
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertActiveFlow(const std::string& flowId) {
    requireRan("assertActiveFlow()");
    const auto& actual = _engine->sessionState().activeFlowId();
    if (actual != flowId) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertActiveFlow failed: expected '"
                                  + flowId + "' got '" + actual + "'");
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertParameter(
    const std::string& name, const std::string& expected) {
    requireRan("assertParameter()");
    auto value = _engine->sessionState().getParameter(name);
    if (!value || value->kind_case() != google::protobuf::Value::kStringValue
        || value->string_value() != expected) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertParameter failed for '" + name
                                  + "': expected string '" + expected + "'");
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertParameter(
    const std::string& name, double expected) {
    requireRan("assertParameter()");
    auto value = _engine->sessionState().getParameter(name);
    if (!value || value->kind_case() != google::protobuf::Value::kNumberValue
        || value->number_value() != expected) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertParameter failed for '" + name
                                  + "': expected number " + std::to_string(expected));
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertParameterMissing(
    const std::string& name) {
    requireRan("assertParameterMissing()");
    if (_engine->sessionState().getParameter(name).has_value()) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertParameterMissing failed for '"
                                  + name + "' - parameter has a value");
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertSessionEnded() {
    requireRan("assertSessionEnded()");
    if (!_result.sessionEnded) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertSessionEnded failed - "
                                  "session did not reach END_SESSION");
    }
    return *this;
}

OrchestratorConversationSimulator& OrchestratorConversationSimulator::assertSaid(const std::string& text) {
    requireRan("assertSaid()");
    auto it = std::find(_result.sayOutputs.begin(), _result.sayOutputs.end(), text);
    if (it == _result.sayOutputs.end()) {
        throw std::runtime_error("[OrchestratorConversationSimulator] assertSaid failed - \"" + text
                                  + "\" was never said");
    }
    return *this;
}

} // namespace orchestrator
} // namespace va
