#include <VASDK/goal/GoalManager.h>
#include <spdlog/spdlog.h>

namespace va {
namespace goal {

//  Construction 

GoalManager::GoalManager(GoalDefinition def, int maxReprompts)
    : _state(std::move(def)), _maxReprompts(maxReprompts) {}

//  Pre-fill 

void GoalManager::prefillSlot(const std::string& dotPath, const nlohmann::json& value) {
    _state.prefillSlot(dotPath, value);
}

//  Wiring 

void GoalManager::attachToLLMTransport(va::llm::LLMTransport* transport) {
    transport->clearHistory();
    spdlog::info("[GoalManager] Activated - goal: '{}' | slots: {}",
                 _state.definition().name, _state.definition().slots.size());
    spdlog::info("[GoalManager] Required slots: {}", _state.missingSlotsSummary());

    transport->registerTemplateVar("goal_objective",
        [this](va::llm::ChatFormat) { return _state.definition().objective; });

    transport->registerTemplateVar("collected_slots",
        [this](va::llm::ChatFormat) { return _state.collectedSlotsSummary(); });

    transport->registerTemplateVar("missing_slots",
        [this](va::llm::ChatFormat) { return _state.missingSlotsSummary(); });

    transport->registerTemplateVar("escalation_hint",
        [this](va::llm::ChatFormat) -> std::string {
            if (_state.repromptCount() < _maxReprompts) return "";
            return "\nIMPORTANT: You have been unable to collect the required information "
                   "after several attempts. Call escalate() with "
                   "reason='max_reprompts_exceeded'.";
        });

    transport->registerTemplateVar("goal_status",
        [this](va::llm::ChatFormat) -> std::string {
            if (_state.isComplete() && _state.isAwaitingConfirmation())
                return "All information collected - awaiting user confirmation";
            if (_state.isComplete())
                return "Goal complete";
            return "Still needed:\n" + _state.missingSlotsSummary();
        });
}

void GoalManager::attach(va::llm::LLMTransport* transport,
                          va::llm::ToolDispatchProcessor* dispatcher) {
    attachToLLMTransport(transport);
    attachToToolDispatcher(dispatcher);
}

void GoalManager::attachToToolDispatcher(va::llm::ToolDispatchProcessor* dispatcher) {
    _dispatcher = dispatcher;
    dispatcher->registerTool(makeUpdateSlotTool());
    dispatcher->registerTool(makeUpdateObjectSlotTool());
    dispatcher->registerTool(makeRequestConfirmationTool());
    dispatcher->registerTool(makeCompleteGoalTool());
    dispatcher->registerTool(makeEscalateTool());
}

//  Callbacks 

void GoalManager::setOnComplete(std::function<void()> fn) {
    _onComplete = std::move(fn);
}

void GoalManager::setOnEscalate(std::function<void(const EscalationContext&)> fn) {
    _onEscalate = std::move(fn);
}

//  State access 

const GoalState& GoalManager::state() const { return _state; }
bool GoalManager::isComplete() const        { return _state.isComplete(); }

void GoalManager::setMetrics(std::weak_ptr<va::metrics::CallMetrics> metrics) {
    _metrics = std::move(metrics);
}

void GoalManager::reset(GoalDefinition newGoal) {
    _state = GoalState(std::move(newGoal));
    spdlog::info("[GoalManager] Goal reset to '{}'", _state.definition().name);
    if (_dispatcher) {
        for (const auto& name : {"update_slot", "update_object_slot",
                                  "request_confirmation", "complete_goal", "escalate"}) {
            _dispatcher->unregisterTool(name);
        }
        _dispatcher->registerTool(makeUpdateSlotTool());
        _dispatcher->registerTool(makeUpdateObjectSlotTool());
        _dispatcher->registerTool(makeRequestConfirmationTool());
        _dispatcher->registerTool(makeCompleteGoalTool());
        _dispatcher->registerTool(makeEscalateTool());
    }
}

//  Tools 

va::llm::Tool GoalManager::makeUpdateSlotTool() {
    return va::llm::Tool("update_slot",
        "Record a scalar value provided by the user "
        "(string, phone, date, enum choice, boolean, number, or COMPOSITE dot-path). "
        "For ENUM slots use exactly one of the allowed values shown in the system prompt. "
        "For DATE/DATETIME use ISO 8601 (YYYY-MM-DD). "
        "Do NOT use this for OBJECT-type slots - use update_object_slot instead.")
        .addParameter({"slot_name", "slot name or dot-path (e.g. 'address.city')", "string", true})
        .addParameter({"value",     "extracted value in the required format",       "string", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            auto slotName = args["slot_name"].get<std::string>();
            auto value    = args["value"].get<std::string>();
            auto result   = _state.updateSlot(slotName, value);
            if (auto m = _metrics.lock()) m->recordSlotAttempt(slotName, result.valid);
            if (!result.valid) {
                _state.incrementRepromptCount();
                spdlog::warn("[GoalManager] update_slot('{}') rejected (reprompts: {}): {}",
                             slotName, _state.repromptCount(), result.errorMessage);
                return {{"success", false}, {"error", result.errorMessage}};
            }
            auto missing = _state.missingSlotsSummary();
            bool complete = _state.isComplete();
            spdlog::info("[GoalManager] Slot '{}' = {} | {}",
                         slotName, result.normalizedValue.dump(),
                         complete ? "ALL SLOTS FILLED" : "still needed: " + missing);
            return {
                {"success",      true},
                {"still_needed", missing},
                {"goal_complete", complete}
            };
        });
}

va::llm::Tool GoalManager::makeUpdateObjectSlotTool() {
    return va::llm::Tool("update_object_slot",
        "Record a structured (multi-field) value provided by the user. "
        "Only use for slots explicitly labelled as OBJECT type in the system prompt. "
        "Pass all known fields as a JSON object; omit optional fields that were not mentioned.")
        .addParameter({"slot_name", "name of the OBJECT-type slot",            "string", true})
        .addParameter({"value",     "the structured value as a JSON object",   "object", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            auto slotName = args["slot_name"].get<std::string>();
            auto result   = _state.updateObjectSlot(slotName, args["value"]);
            if (auto m = _metrics.lock()) m->recordSlotAttempt(slotName, result.valid);
            if (!result.valid) {
                _state.incrementRepromptCount();
                spdlog::warn("[GoalManager] update_object_slot('{}') rejected (reprompts: {}): {}",
                             slotName, _state.repromptCount(), result.errorMessage);
                return {{"success", false}, {"error", result.errorMessage}};
            }
            auto missing = _state.missingSlotsSummary();
            bool complete = _state.isComplete();
            spdlog::info("[GoalManager] Object slot '{}' = {} | {}",
                         slotName, args["value"].dump(),
                         complete ? "ALL SLOTS FILLED" : "still needed: " + missing);
            return {
                {"success",      true},
                {"still_needed", missing},
                {"goal_complete", complete}
            };
        });
}

va::llm::Tool GoalManager::makeEscalateTool() {
    return va::llm::Tool("escalate",
        "Escalate to an external handler when this goal cannot or should not be completed here. "
        "Use when: the user explicitly requests it; the goal cannot be completed after multiple "
        "attempts; or the situation is outside the scope of the current process.")
        .addParameter({"reason", "brief reason for escalation", "string", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            EscalationContext ctx;
            ctx.reason         = args["reason"].get<std::string>();
            ctx.collectedSlots = _state.toJson();

            spdlog::info("[GoalManager] Escalating - reason: '{}'", ctx.reason);

            if (_onEscalate) _onEscalate(ctx);

            return {{"status",      "escalated"},
                    {"instruction", "Inform the user that this will be handled through a different process."}};
        });
}

va::llm::Tool GoalManager::makeRequestConfirmationTool() {
    return va::llm::Tool("request_confirmation",
        "Call this when ALL required slots are filled, before calling complete_goal(). "
        "Provide a natural recap of everything collected so the user can confirm "
        "or correct any detail.")
        .addParameter({"recap", "natural-language summary of all collected information", "string", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            _state.setAwaitingConfirmation(true);
            _state.incrementConfirmationAttempts();
            auto recap = args.value("recap", std::string{});
            spdlog::info("[GoalManager] Confirmation requested (attempt {}) - recap: \"{}\"",
                         _state.confirmationAttempts(), recap);
            return {
                {"status", "awaiting_confirmation"},
                {"instruction",
                 "Wait for the user to respond. "
                 "If confirmed: call complete_goal(). "
                 "If corrected: call update_slot/update_object_slot, then request_confirmation() again."}
            };
        });
}

va::llm::Tool GoalManager::makeCompleteGoalTool() {
    return va::llm::Tool("complete_goal",
        "Call this ONLY when ALL required slots are filled. "
        "Triggers the external completion action. "
        "After calling this, speak a natural closing statement to the user.")
        .addParameter({"summary", "brief natural-language summary of what was completed", "string", true})
        .onCall([this](const nlohmann::json& args) -> nlohmann::json {
            if (!_state.isComplete()) {
                spdlog::warn("[GoalManager] complete_goal called but slots incomplete - still needed: {}",
                             _state.missingSlotsSummary());
                return {{"success", false},
                        {"error", "Not all required slots are filled. Still needed: "
                                  + _state.missingSlotsSummary()}};
            }
            auto slots = _state.toJson();
            spdlog::info("[GoalManager] Completing goal '{}' - collected slots: {}",
                         _state.definition().name, slots.dump(2));
            bool ok = false;
            if (_state.definition().onComplete) {
                ok = _state.definition().onComplete(slots);
            } else {
                ok = true;
            }

            spdlog::info("[GoalManager] Goal '{}' {} - summary: \"{}\"",
                         _state.definition().name, ok ? "COMPLETED" : "FAILED (onComplete returned false)",
                         args.value("summary", std::string{}));

            if (ok && _onComplete) {
                _onComplete();
            }
            return {{"success", ok}};
        });
}

} // namespace goal
} // namespace va
