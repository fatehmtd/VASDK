// Goal layer: slot validation, conditional COMPOSITE sub-slots, enforced confirmation, the
// JSON loader, persistence round-trip, keeping history on attach, and the simulator actually
// running its scripted tool calls.

#include <VASDK/goal/ConditionEvaluator.h>
#include <VASDK/goal/GoalConversationSimulator.h>
#include <VASDK/goal/GoalDefinitionLoader.h>
#include <VASDK/goal/GoalManager.h>
#include <VASDK/goal/GoalStatePersistence.h>
#include <VASDK/llm/OpenAITransport.h>

#include <cstdio>
#include <nlohmann/json.hpp>

using namespace va;
using nlohmann::json;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

goal::GoalSlot slot(std::string name, goal::SlotType type, bool required = true) {
    goal::GoalSlot s;
    s.name = std::move(name);
    s.type = type;
    s.required = required;
    return s;
}

// address.unit is only required when building_type == "apartment"
goal::GoalDefinition addressGoal() {
    goal::GoalDefinition def;
    def.name = "address";
    auto building = slot("building_type", goal::SlotType::ENUM);
    building.constraints.allowedValues = {"house", "apartment"};
    auto address = slot("address", goal::SlotType::COMPOSITE);
    address.subSlots.push_back(slot("street", goal::SlotType::STRING));
    auto unit = slot("unit", goal::SlotType::STRING);
    unit.condition = goal::compileCondition({{"slot", "building_type"}, {"op", "eq"}, {"value", "apartment"}});
    address.subSlots.push_back(unit);
    def.slots = {building, address};
    return def;
}

void testPhoneNormalisation() {
    goal::GoalDefinition def;
    def.name = "contact";
    def.slots = {slot("phone", goal::SlotType::PHONE)};
    goal::GoalState state(def);
    check(state.updateSlot("phone", "555 123 4567").valid, "phone: 10 digits accepted");
    check(state.slotValueStr("phone") == "555-123-4567", "phone: normalised");
    check(!state.updateSlot("phone", "12345").valid, "phone: 5 digits rejected");
}

void testCompositeSubSlotCondition() {
    goal::GoalState house(addressGoal());
    house.updateSlot("building_type", "house");
    house.updateSlot("address.street", "1 Main St");
    check(house.isComplete(), "composite: unit not required for a house");

    goal::GoalState flat(addressGoal());
    flat.updateSlot("building_type", "apartment");
    flat.updateSlot("address.street", "1 Main St");
    check(!flat.isComplete(), "composite: unit required for an apartment");
    check(flat.missingSlotsSummary().find("unit") != std::string::npos, "composite: unit listed as missing");
    flat.updateSlot("address.unit", "4B");
    check(flat.isComplete(), "composite: complete once unit given");
}

void testConfirmationEnforced() {
    bool completed = false;
    goal::GoalDefinition def;
    def.name = "booking";
    def.slots = {slot("name", goal::SlotType::STRING)};
    def.requireConfirmation = true;
    def.onComplete = [&](const json&) { completed = true; return true; };

    auto update   = [](const char* v) { return goal::SimToolCall{"update_slot", {{"slot_name", "name"}, {"value", v}}}; };
    auto confirm  = goal::SimToolCall{"request_confirmation", {{"recap", "booking for Alice"}}};
    auto complete = goal::SimToolCall{"complete_goal", {{"summary", "booked"}}};

    goal::GoalConversationSimulator unconfirmed(def);
    unconfirmed.addTurn({"Alice", {update("Alice"), complete}}).run();
    check(!completed, "confirmation: complete_goal refused before request_confirmation");

    goal::GoalConversationSimulator corrected(def);
    corrected.addTurn({"Alice", {update("Alice"), confirm}})
             .addTurn({"actually Bob", {update("Bob"), complete}}).run();
    check(!completed, "confirmation: a correction makes the earlier confirmation stale");

    goal::GoalConversationSimulator confirmed(def);
    confirmed.addTurn({"Alice", {update("Alice"), confirm}})
             .addTurn({"yes", {complete}}).run();
    check(completed, "confirmation: complete_goal accepted after confirmation");
    confirmed.assertSlotValue("name", "Alice");

    bool completedWithoutFlag = false;
    def.requireConfirmation = false;
    def.onComplete = [&](const json&) { completedWithoutFlag = true; return true; };
    goal::GoalConversationSimulator legacy(def);
    legacy.addTurn({"Alice", {update("Alice"), complete}}).run();
    check(completedWithoutFlag, "confirmation: optional by default (old behaviour)");
}

void testLoader() {
    auto def = goal::GoalDefinitionLoader::fromJson(json::parse(R"({
        "name": "address", "requireConfirmation": true,
        "slots": [
          { "name": "building_type", "type": "ENUM", "constraints": { "allowedValues": ["house", "apartment"] } },
          { "name": "address", "type": "COMPOSITE", "subSlots": [
              { "name": "street" },
              { "name": "unit", "condition": { "slot": "building_type", "op": "eq", "value": "apartment" } } ] } ] })"));
    check(def.requireConfirmation, "loader: requireConfirmation parsed");
    check(!goal::GoalDefinitionLoader::fromJson({{"name", "x"}}).requireConfirmation, "loader: requireConfirmation defaults to false");

    goal::GoalState state(def);
    state.updateSlot("building_type", "house");
    state.updateSlot("address.street", "1 Main St");
    check(state.isComplete(), "loader: sub-slot condition from JSON honoured");
}

void testPersistenceRoundTrip() {
    goal::GoalState state(addressGoal());
    state.updateSlot("building_type", "house");
    state.setAwaitingConfirmation(true);
    auto snapshot = goal::serializeGoalState(state);
    auto restored = goal::deserializeGoalState(addressGoal(), snapshot);
    check(restored.slotValueStr("building_type") == "house", "persistence: slot value restored");
    check(restored.isAwaitingConfirmation(), "persistence: confirmation flag restored");
}

void testAttachKeepsHistoryWhenAsked() {
    llm::LLMConfig cfg;
    cfg.endpoint = "http://localhost:1/v1/chat/completions";
    llm::OpenAITransport transport(cfg);
    llm::ToolDispatchProcessor dispatcher;

    transport.addMessageToHistory(llm::MessageRole::USER, "hello");
    goal::GoalManager keeps(addressGoal());
    goal::AttachOptions keepHistory;
    keepHistory.clearHistory = false;
    keeps.attach(&transport, &dispatcher, keepHistory);
    check(transport.getHistory().size() == 1, "attach: history kept with clearHistory=false");

    goal::GoalManager clears(addressGoal());
    clears.attach(&transport, &dispatcher);
    check(transport.getHistory().empty(), "attach: history cleared by default");
}

} // namespace

int main() {
    testPhoneNormalisation();
    testCompositeSubSlotCondition();
    testConfirmationEnforced();
    testLoader();
    testPersistenceRoundTrip();
    testAttachKeepsHistoryWhenAsked();
    if (failures == 0) std::printf("goal_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
