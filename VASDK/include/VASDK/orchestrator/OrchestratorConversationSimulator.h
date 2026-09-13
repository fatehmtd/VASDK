#pragma once

#include "vasdk_export.h"
#include <VASDK/orchestrator/OrchestratorEngine.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace va {
namespace orchestrator {

// A single scripted conversation turn - just the utterance text.
// setInjectedNLUResult below (Milestone 2.3, spec §12) is how a test attaches
// an NLUResult to a specific turn, keyed by that turn's exact userText -
// following spec §12/§20.2's own sim.setInjectedNLUResult(utterance, result)
// shape rather than adding a field to this struct, since it lets the same
// injected result cover a repeated utterance without re-specifying it.
struct SimTurn {
    std::string userText;
};

// What a run() actually produced - checked by the assert* methods below, and
// available directly for callers who want raw access instead.
struct SimulationResult {
    bool sessionEnded = false;
    std::string completedFlowId;         // set iff sessionEnded, via onFlowComplete
    std::vector<std::string> sayOutputs; // every `say` action's text, firing order, whole run
};

// Milestone 1.8 (docs/reference/requestflow-roadmap.md §3) - mirrors
// va::goal::GoalConversationSimulator's scripted-turn design (spec §12),
// driving a real OrchestratorEngine end-to-end with no audio device, no
// model, no live webhook. Scoped to exactly what M1.6/M1.7 implement:
// event/condition routes, the four inline action kinds, and session
// snapshot/restore - see SimTurn above for the one deliberate gap (no
// injected NLU result); OrchestratorEngine's own class comment documents the
// rest of the engine-level scope this simulator inherits unchanged.
class VASDK_EXPORT OrchestratorConversationSimulator {
public:
    explicit OrchestratorConversationSimulator(va::orchestrator::Agent agent);

    // Resumes a previously-snapshotted session (spec §10/§16's snapshot/
    // restore acceptance criterion) - same Agent config, restored runtime
    // state, mirroring OrchestratorEngine's own restoring constructor. run()
    // treats this instance as already started, exactly like the engine does.
    OrchestratorConversationSimulator(va::orchestrator::Agent agent,
                                       const va::orchestrator::SessionSnapshot& snapshot);

    OrchestratorConversationSimulator& addTurn(SimTurn turn);
    OrchestratorConversationSimulator& addTurn(std::string userText);

    // Milestone 2.3 (spec §12/§20.2) - the next call to run() will pass
    // `result` as the injected NLUResult for any turn whose userText equals
    // `utterance` exactly, bypassing whatever real NLUProvider is attached
    // (see setNLUProvider below) for that turn only. This is what makes
    // intent-route tests deterministic and model-free.
    OrchestratorConversationSimulator& setInjectedNLUResult(const std::string& utterance,
                                                             va::orchestrator::NLUResult result);

    // Attaches a real NLUProvider (M2.1's TrainingPhraseNLUProvider, M2.2's
    // LLMNLUProvider, or a custom one) for turns that have no injected result
    // - passed straight through to the underlying OrchestratorEngine once
    // run() constructs it.
    OrchestratorConversationSimulator& setNLUProvider(std::shared_ptr<va::orchestrator::NLUProvider> provider);

    // Starts the session (unless this instance was constructed from a
    // snapshot - a restored session is already started) and feeds every
    // scripted turn to the engine in order, synchronously. Stops early if a
    // turn ends the session.
    SimulationResult run();

    // Spec §10 - the other half of the restore constructor above. Callable
    // only after run(), same precondition as the assert* methods below.
    va::orchestrator::SessionSnapshot snapshot() const;

    //  Fluent assertions (call after run()) - throw std::runtime_error on
    // failure, matching GoalConversationSimulator's convention so a failed
    // assertion reads as a normal test failure, not a silent false return.
    OrchestratorConversationSimulator& assertActiveNode(const std::string& nodeId);
    OrchestratorConversationSimulator& assertActiveFlow(const std::string& flowId);
    OrchestratorConversationSimulator& assertParameter(const std::string& name, const std::string& expected);
    OrchestratorConversationSimulator& assertParameter(const std::string& name, double expected);
    OrchestratorConversationSimulator& assertParameterMissing(const std::string& name);
    OrchestratorConversationSimulator& assertSessionEnded();
    OrchestratorConversationSimulator& assertSaid(const std::string& text);

private:
    va::orchestrator::Agent _agent;
    std::optional<va::orchestrator::SessionSnapshot> _restoreFrom;
    std::vector<SimTurn> _turns;
    SimulationResult _result;
    bool _ran = false;

    std::shared_ptr<OrchestratorEngine> _engine;
    std::shared_ptr<va::orchestrator::NLUProvider> _nluProvider;
    std::unordered_map<std::string, va::orchestrator::NLUResult> _injectedNluResults;

    void requireRan(const char* what) const;
};

} // namespace orchestrator
} // namespace va
