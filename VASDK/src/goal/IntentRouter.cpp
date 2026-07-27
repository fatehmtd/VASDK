#include <VASDK/goal/IntentRouter.h>
#include <VASDK/llm/PromptTemplate.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace va {
namespace goal {

IntentRouter::IntentRouter(std::vector<IntentOption> options, double confidenceThreshold)
    : _options(std::move(options)), _confidenceThreshold(confidenceThreshold) {}

void IntentRouter::setRoleDescription(std::string description) {
    _roleDescription = std::move(description);
}

//  Prompt

void IntentRouter::buildIntentPrompt(va::llm::LLMTransport* transport) {
    std::ostringstream ss;
    ss << _roleDescription << "\n"
       << "Listen to the user's request and call classify_intent() immediately.\n"
       << "Do not ask clarifying questions before classifying - infer from what you hear.\n\n"
       << "Available intents:\n";
    for (const auto& opt : _options) {
        ss << "- " << opt.name << ": " << opt.description << "\n";
    }
    ss << "\nAvailable tools:\n{tool_descriptions}";

    auto promptTemplate = va::llm::PromptTemplate::create(transport->getPromptChatFormat());
    promptTemplate->setMissingVarPolicy(va::llm::MissingVarPolicy::LEAVE_PLACEHOLDER);
    promptTemplate->addSystem(ss.str());
    transport->setPromptTemplate(std::move(promptTemplate));
}

//  Wiring 

void IntentRouter::attachToLLMTransport(va::llm::LLMTransport* transport) {
    _transport = transport;
    buildIntentPrompt(transport);
}

void IntentRouter::attachToToolDispatcher(va::llm::ToolDispatchProcessor* dispatcher) {
    _dispatcher = dispatcher;
    dispatcher->registerTool(makeClassifyIntentTool());
}

void IntentRouter::setOnIntent(
    std::function<void(const std::string&, const GoalDefinition&)> fn) {
    _onIntent = std::move(fn);
}

bool IntentRouter::intentClassified() const { return _intentClassified; }

const GoalDefinition* IntentRouter::goalForIntent(const std::string& name) const {
    for (const auto& opt : _options) {
        if (opt.name == name) return &opt.goal;
    }
    return nullptr;
}

void IntentRouter::reclassify(const std::string& intentName) {
    const GoalDefinition* goal = goalForIntent(intentName);
    if (!goal) {
        spdlog::warn("[IntentRouter] reclassify: unknown intent '{}'", intentName);
        return;
    }
    spdlog::info("[IntentRouter] Reclassifying to intent '{}'", intentName);
    _intentClassified = true;
    if (_onIntent) _onIntent(intentName, *goal);
}

//  Tools 

va::llm::Tool IntentRouter::makeClassifyIntentTool() {
    return va::llm::Tool("classify_intent",
        "Call as soon as the user's intent is clear enough to determine which goal to pursue. "
        "If confidence is too low, ask the user to clarify instead.")
        .addParameter({"intent",     "one of the available intent names", "string", true})
        .addParameter({"confidence", "confidence score 0.0-1.0",          "number", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            auto intent     = args["intent"].get<std::string>();
            auto confidence = args["confidence"].get<double>();

            if (confidence < _confidenceThreshold) {
                spdlog::info("[IntentRouter] Low-confidence classification - intent='{}' conf={:.2f} (threshold {:.2f}) - asking user to clarify",
                             intent, confidence, _confidenceThreshold);
                return {{"status",      "low_confidence"},
                        {"instruction", "Ask the user to clarify what they need."}};
            }

            const GoalDefinition* goal = goalForIntent(intent);
            if (!goal) {
                spdlog::warn("[IntentRouter] Unknown intent '{}' returned by model", intent);
                return {{"status", "unknown_intent"},
                        {"instruction", "Unknown intent '" + intent + "'. Ask the user to clarify."}};
            }

            spdlog::info("[IntentRouter] Intent classified: '{}' (confidence {:.2f})", intent, confidence);
            _intentClassified = true;

            if (_onIntent) _onIntent(intent, *goal);

            return {{"status", "classified"}, {"intent", intent}};
        });
}

void IntentRouter::registerSwitchGoalTool(va::llm::ToolDispatchProcessor* dispatcher) {
    dispatcher->registerTool(
        va::llm::Tool("switch_goal",
            "Call when the user explicitly changes what they need mid-conversation. "
            "Provide the new intent name from the available options.")
            .addParameter({"new_intent", "the intent name the user now wants", "string", true})
            .onCall([this](const nlohmann::json& args) -> nlohmann::json {
                auto newIntent = args["new_intent"].get<std::string>();
                reclassify(newIntent);
                return {{"status",      "goal_switched"},
                        {"instruction", "Acknowledge the change and proceed with the new goal."}};
            })
    );
}

} // namespace goal
} // namespace va
