# Example apps

Four apps show VASDK end to end. Each is a working program you can run, read and copy from, and each guide explains how it's put together.

| App | What it is | Read it for |
| --- | --- | --- |
| [VASDK_Pipeline](pipeline.md) | A general-purpose desktop assistant: mic to speaker, barge-in, tools, metrics, a terminal UI, browser calls | The voice loop, turn-taking, providers chosen by config, an agent in front of an LLM |
| [VASDK_CallCenter](callcenter.md) | A phone call answered by agent files: a routing page, forms, CRM tools, a handoff to a person | Goal-oriented conversations with no LLM, forms with read-backs, tests by text, a call log |
| [VASDK_Car](car.md) | An in-car assistant built from ten self-contained verticals, with a live dashboard | A large agent in parts, triggers from sensors, rules and an LLM together, a model per task |
| [Android](android.md) | The full stack on a phone, in Kotlin over JNI: local models, device tools, background triggers | Embedding VASDK in a mobile app, tools with confirmation, on-device tests |

Which one to start from:

- **A voice assistant that mostly chats**, with a few tools: VASDK_Pipeline.
- **A task-completing agent** (bookings, account questions, intake) that must work without an LLM: VASDK_CallCenter.
- **Many features on one device**, commands that must be instant and offline, an LLM for the rest: VASDK_Car.
- **A phone app:** Android.

The same agent features run in all four, and from other languages through the [C API](../reference/c-api.md) and its [Python binding](../../bindings/python/README.md).

## What each app uses

| Feature | Pipeline | CallCenter | Car | Android |
| --- | --- | --- | --- | --- |
| [Agent files](../reference/flows.md): pages, intents, routes | garage menu | the call flow | ten verticals | phone calls |
| [Forms](../reference/flows.md#forms) with read-back | booking | scheduling, account | texts | |
| [Entities](../reference/flows.md#entities) written in code | | | contacts, places | |
| [Triggers](../reference/flows.md#triggers): the assistant speaks first | | | calls, texts, fuel, cabin, arrival, driver | schedules, in the background |
| [An agent in several files](../reference/flows.md#entities) (`include_files`) | | | one per vertical | |
| [Handing topics to the LLM](../reference/flows.md#with-an-llm) (`target: "LLM"`) | | | stories, games, questions | |
| [A model per task](../reference/engine-config.md#different-llms-for-different-tasks) (`llm_profiles`) | example config | | fast, default, smart | |
| [Tools](../reference/llm.md#tool) for the LLM | weather, time | CRM | the car's controls | device tools, with confirmation |
| Browser calls over [WebRTC](../reference/transports.md#webrtctransport) | `--webrtc-port` | `--webrtc-port` | `--webrtc` | |
| [Many conversations at once](../reference/engine.md#many-conversations-at-once) | | an agent per number | | |
