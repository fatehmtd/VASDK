# Example: defining a goal in JSON

A `GoalDefinition` describes a task the assistant must complete with the caller: a name, an objective shown to the LLM, and the slots (pieces of information) to collect. Goals can be built in C++ or loaded from JSON with `GoalDefinitionLoader`, which is useful for handing call-flow changes to whoever owns the spec instead of recompiling.

## The JSON

```json
{
  "name": "schedule_callback",
  "objective": "Collect the caller's phone number and a preferred callback window.",
  "slots": [
    { "name": "phone", "type": "PHONE", "description": "Caller's callback number" },
    { "name": "window", "type": "ENUM", "description": "Preferred time of day",
      "constraints": { "allowedValues": ["morning", "afternoon", "evening"] } }
  ]
}
```

## Loading and wiring it

The `onComplete` callback can't be serialized, so it's attached in C++ after loading:

```cpp
#include <VASDK/goal/GoalDefinitionLoader.h>
#include <VASDK/goal/GoalManager.h>

auto goal = va::goal::GoalDefinitionLoader::fromFile("goals/schedule_callback.json");
goal.onComplete = [](const nlohmann::json& slots) {
    return crm.scheduleCallback(slots.at("phone"), slots.at("window"));
};

va::goal::GoalManager manager(std::move(goal));
manager.attach(llmTransport.get(), toolDispatcher.get());
```

`attach()` does two things: it registers prompt template variables on the transport (`{goal_objective}`, `{collected_slots}`, `{missing_slots}`, `{goal_status}`, `{escalation_hint}`, refreshed on every LLM request) and registers five tools on the dispatcher: `update_slot`, `update_object_slot`, `request_confirmation`, `complete_goal`, and `escalate`. The LLM fills slots by calling tools; validation failures come back as tool errors so the model can re-prompt the caller.

`GoalDefinitionLoader::fromDirectory("goals/")` loads every `.json` in a directory, which pairs naturally with `IntentRouter` when you have one goal per intent.

## Slot types

| Type | Behavior |
| --- | --- |
| `STRING` | Free text, passed through |
| `PHONE` | Normalized to `NXX-NXX-XXXX` (US) or `NXX-XXXX` |
| `EMAIL` | Basic format validation |
| `DATE` / `TIME` / `DATETIME` | ISO 8601; `constraints.mustBeFuture` supported |
| `NUMBER` / `INTEGER` | Optional `min`/`max` constraints |
| `BOOLEAN` | Accepts yes/no/true/false/1/0 |
| `ENUM` | Must match one of `constraints.allowedValues` (case-insensitive) |
| `OBJECT` | Whole JSON object set at once via `update_object_slot`, validated against `constraints.objectSchema` |
| `COMPOSITE` | Named `subSlots` filled one at a time via dot-path notation |

Slots can also be conditional: a `condition` (a lambda in C++, or the condition DSL compiled by `ConditionEvaluator` when loading from JSON) makes a slot required only when it evaluates true against the live `GoalState`. The call center app uses this to require `vehicle_info` only when `service_type` is `repair`.

## Full reference

[`VASDK_CALLCENTER_APP/callcenter_pipeline.cpp`](../../VASDK_CALLCENTER_APP/callcenter_pipeline.cpp) builds two complete goals in C++ (including an OBJECT slot with a schema and a conditional requirement) and wires them through `IntentRouter`. See the [call center app guide](../../VASDK_CALLCENTER_APP/README.md).
