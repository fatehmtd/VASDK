#pragma once

#include "vasdk_export.h"
#include <VASDK/goal/GoalDefinition.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <functional>
#include <string>
#include <vector>

namespace va {
namespace goal {

struct IntentOption {
    std::string    name;        ///< e.g. "schedule_callback"
    std::string    description; ///< e.g. "user wants to schedule an appointment"
    GoalDefinition goal;
};

class VASDK_EXPORT IntentRouter {
public:
    explicit IntentRouter(std::vector<IntentOption> options,
                          double confidenceThreshold = 0.6);

    //  Configuration

    /// Override the opening sentence of the intent-detection system prompt.
    /// Defaults to "You are a helpful assistant." - set this to something domain-specific
    /// (e.g. "You are a call center assistant.") before calling attachToLLMTransport.
    void setRoleDescription(std::string description);

    //  Wiring

    /// Sets the intent-detection PromptTemplate and registers classify_intent tool.
    void attachToLLMTransport(va::llm::LLMTransport* transport);
    void attachToToolDispatcher(va::llm::ToolDispatchProcessor* dispatcher);

    //  Callbacks 

    /// Called when an intent is classified with sufficient confidence.
    /// The app uses this to create + attach a GoalManager for the selected intent.
    void setOnIntent(std::function<void(const std::string& intentName,
                                        const GoalDefinition& goal)> fn);

    /// Registers a switch_goal tool so the LLM can change goal mid-call.
    void registerSwitchGoalTool(va::llm::ToolDispatchProcessor* dispatcher);

    bool intentClassified() const;

    /// Look up the GoalDefinition for a given intent name (used inside setOnIntent callback).
    const GoalDefinition* goalForIntent(const std::string& name) const;

    /// Programmatically trigger re-classification to a specific intent (Phase 4).
    void reclassify(const std::string& intentName);

private:
    std::vector<IntentOption> _options;
    double                    _confidenceThreshold;
    bool                      _intentClassified = false;
    std::string               _roleDescription  = "You are a helpful assistant.";

    std::function<void(const std::string&, const GoalDefinition&)> _onIntent;

    va::llm::LLMTransport*            _transport    = nullptr;
    va::llm::ToolDispatchProcessor*   _dispatcher   = nullptr;

    va::llm::Tool makeClassifyIntentTool();
    void          buildIntentPrompt(va::llm::LLMTransport* transport);
};

} // namespace goal
} // namespace va
