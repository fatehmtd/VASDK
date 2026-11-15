---
name: vasdk-agent
description: Write a VASDK agent - a scripted conversation in an agent file (pages, intents, routes, forms and goals, entities, triggers, stored values, languages, include_files, handing turns to the LLM), with C++/C/Python extensions, checked and tested with vasdk_agentc (lint, test, chat, eval). Use when building or changing a conversation, a form, an intent, or an agent's tests.
---

# An agent

An agent answers by rules: pages with routes, intents, forms that fill goals, entities, triggers. It runs with no LLM; with one, it hands over what it doesn't answer itself (`target: "LLM"`). Everything is in text-proto files, checked and tested without a microphone.

| | |
| --- | --- |
| Reference | [Agents](../../docs/reference/flows.md), [goals](../../docs/reference/goal.md), [languages](../../docs/reference/languages.md) |
| Schemas | `VASDK/proto/vasdk/flow/agent.proto`, `VASDK/proto/vasdk/flow/test_case.proto`, `VASDK/proto/vasdk/goal/goal.proto` |
| Runtime | `VASDK/include/VASDK/flow/CompiledAgent.h`, `VASDK/include/VASDK/flow/FlowRegistry.h` (steps, page types, tools in C++) |
| Tool | `VASDK/tools/vasdk_agentc/`: `lint`, `test`, `chat`, `eval` |
| Worked example | `VASDK_APP/agents/garage-menu/` |

## Start from the example

```text
VASDK_APP/agents/garage-menu/
  agent.txtpb              flows, pages, intents, routes, entities
  goals/booking.txtpb      a form's slots and validation
  garage.proto             the agent's own message types
  extensions/              C++ steps, page types, tools + garage_agentc.cpp (vasdk_agentc with them registered)
  tests/*.txtpb            FlowTestCase conversations, by text
  nlu_eval.txtpb           what callers say, for intent accuracy and entity F1
  nlu_eval.spoken.txtpb    the same as STT heard it
```

Larger agents split into parts with `include_files` (the car's `VASDK_CAR_APP/agents/car/agent.txtpb` includes one part per vertical). Another language: [an agent in another language](../../docs/examples/languages.md).

## The loop

```bash
garage_agentc lint  agents/<name>/agent.txtpb                       # errors stop loading; warnings don't
garage_agentc expand agents/<name>/agent.txtpb [intent] [--collisions]   # what the patterns spell; sentences two intents share
garage_agentc test  agents/<name>/agent.txtpb agents/<name>/tests/*.txtpb
garage_agentc chat  agents/<name>/agent.txtpb                       # try it by text
garage_agentc eval  agents/<name>/agent.txtpb agents/<name>/nlu_eval.txtpb --sweep
```

An agent with no C++ extensions uses `vasdk_agentc` itself; one with extensions needs its own copy that registers them (`flow::runAgentTool(argc, argv, registry)`, see `VASDK_APP/agents/garage-menu/extensions/garage_agentc.cpp`). Details: [checking and testing](../../docs/reference/flows.md#checking-and-testing-vasdk_agentc).

## Pitfalls

- Every turn goes through [how a turn is answered](../../docs/reference/flows.md#how-a-turn-is-answered): rules, then the sentence encoder, then the LLM. Test the paraphrases users really say, not only the examples.
- Write examples as [patterns and phrase sets](../../docs/reference/flows.md#patterns-and-phrase-sets) (`"(turn|switch) on [the] ${light}"`), not long lists, and check them with `expand`. Sets belong to a file (or an intent); share wording between files with a pack (`phrase_set_files`), never by assuming another part's set. Prefer `_` or a phrase set to `*`, which matches loosely. Fix or explain every collision lint reports, and when rewriting examples, compare `expand` before and after so no sentence is lost. Worked example: [device controls](../../docs/examples/device-controls.md).
- Device commands (a light, a volume) belong in the agent, not left to a small on-device LLM, which may answer in words instead of calling its tool.
- A value with a range or with levels is a [bounded NUMBER or an ENUM](../../docs/reference/flows.md#bounded-numbers-and-enums) (`min`/`max`/`step`, `entity`), moved with `increase`/`decrease`, with an `at-limit` handler that says where it stopped; not arithmetic in every route. Choose `out_of_range` on purpose: unset it neither clamps nor rejects. Give an ENUM's entity `matching { fuzzy: false }` when its synonyms are close to the command words.
- Private answers (card numbers, PINs) are marked sensitive so logs and off-device models never see them ([tools and security](../../docs/reference/flows.md#tools-and-security)).
- A feature the agent language lacks belongs in the core (`FlowRegistry` page types and steps, or a new field in `agent.proto`), usable by every agent, not in one app's code ([house rules](../../AGENTS.md#house-rules)).
- Steps, page types and tools written in C++ also need C and Python equivalents to stay usable from every platform ([in other languages](../../docs/reference/flows.md#in-other-languages)).

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- **Conversations** (`tests/*.txtpb`): one file per behaviour; cover the happy path, a correction mid-form, a not-understood turn, a digression and back, a handoff. Registered in the app's CMakeLists, like `vasdk_agent_garage_menu` in `VASDK_APP/CMakeLists.txt`.
- **Understanding** (`nlu_eval.txtpb`): registered with `eval ... --min-accuracy`, like `vasdk_agent_garage_menu_nlu`.
- **Extensions** (C++): a unit test of each step or tool against the agent, like the car's `vasdk_car_<vertical>_code`.
- **Integration:** the agent in a spoken session, through the C API or Python (`bindings/python/tests/test_garage.py` runs the garage suite from Python).

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds: `lint` is clean of errors, every test case and eval passes in `ctest`, and the app's docs describe what the agent does.
