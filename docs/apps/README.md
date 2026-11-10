# Example apps

Five apps show VASDK end to end. Each is a working program you can run, read and copy from, and each guide explains how it's put together.

| App | What it is | Read it for |
| --- | --- | --- |
| [VASDK_Pipeline](pipeline.md) | A general-purpose desktop assistant: mic to speaker, barge-in, tools, metrics, a terminal UI, browser calls | The voice loop, turn-taking, providers chosen by config, an agent in front of an LLM |
| [VASDK_CallCenter](callcenter.md) | A phone call answered by agent files: a routing page, forms, CRM tools, a handoff to a person | Goal-oriented conversations with no LLM, forms with read-backs, tests by text, a call log |
| [VASDK_Car](car.md) | An in-car assistant built from ten self-contained verticals, with a live dashboard | A large agent in parts, triggers from sensors, rules and an LLM together, a model per task |
| [Android](android.md) | The full stack on a phone, in Kotlin over JNI: local models, device tools, background triggers | Embedding VASDK in a mobile app, tools with confirmation, on-device tests |
| [VASDK_PYTHON_APP](python.md) | A kitchen assistant in Python: recipes step by step, timers, a shopping list, cooking questions | VASDK from Python: spoken sessions, the agent's tools and entities and the LLM's tools in Python, triggers fired from Python |

Which one to start from:

- **A voice assistant that mostly chats**, with a few tools: VASDK_Pipeline.
- **A task-completing agent** (bookings, account questions, intake) that must work without an LLM: VASDK_CallCenter.
- **Many features on one device**, commands that must be instant and offline, an LLM for the rest: VASDK_Car.
- **A phone app:** Android.
- **Python:** VASDK_PYTHON_APP.

The three C++ apps are optional parts of the build, off by default, so a plain build is the SDK alone. Turn them on with `-DVASDK_BUILD_APPS=ON` (or `cmake --preset apps`), or one at a time with `VASDK_BUILD_APP`, `VASDK_BUILD_CALLCENTER` and `VASDK_BUILD_CAR`. A plain `cmake --install` leaves them out; `cmake --install build --component apps` installs them.

The same agent features run in all five. VASDK_PYTHON_APP reaches them through the [C API](../reference/c-api.md)'s [Python binding](../../bindings/python/README.md), which any other language can do the same way.

## What each app uses

| Feature | Pipeline | CallCenter | Car | Android | Python |
| --- | --- | --- | --- | --- | --- |
| [Agent files](../reference/flows.md): pages, intents, routes | garage menu | the call flow | ten verticals | phone calls | kitchen, in five parts |
| [Forms](../reference/flows.md#forms) with read-back | booking | scheduling, account | texts | | timer length (no read-back) |
| [Entities](../reference/flows.md#entities) written in code | | | contacts, places | | recipes, ingredients (Python) |
| [Triggers](../reference/flows.md#triggers): the assistant speaks first | | | calls, texts, fuel, cabin, arrival, driver | schedules, in the background | timers (fired from Python), inactivity |
| [An agent in several files](../reference/flows.md#entities) (`include_files`) | | | one per vertical | | one per part |
| [Handing topics to the LLM](../reference/flows.md#with-an-llm) (`target: "LLM"`) | | | stories, games, questions | | cooking questions, unknown items |
| [A model per task](../reference/engine-config.md#different-llms-for-different-tasks) (`llm_profiles`) | example config | | fast, default, smart | | fast, default, smart |
| [Tools](../reference/llm.md#tool) for the LLM | weather, time | CRM | the car's controls | device tools, with confirmation | conversions, timers, the list (Python) |
| Browser calls over [WebRTC](../reference/transports.md#webrtctransport) | `--webrtc-port` | `--webrtc-port` | `--webrtc` | | |
| [Many conversations at once](../reference/engine.md#many-conversations-at-once) | | an agent per number | | | the demo: two sessions |
