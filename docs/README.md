# VASDK documentation

Start here and pick your path.

## By audience

| You are… | Start with |
| --- | --- |
| **Evaluating VASDK** (PM, project lead, architect doing a first pass) | [Plain-language overview](overview.md) - what it is, what you can build, maturity, licensing. No code. |
| **New to the codebase** (any level, including junior) | Root [README](../README.md) for the quick start, then [Core concepts](#core-concepts-reference) below in order: frames → processors → pipeline. The [minimal pipeline example](examples/minimal-pipeline.md) is the fastest way to a mental model. |
| **Building an app on VASDK** | The [example apps](apps/README.md) and which to start from: [VASDK_Pipeline](apps/pipeline.md) (general assistant), [VASDK_CallCenter](apps/callcenter.md) (goal-oriented, no LLM needed), [VASDK_Car](apps/car.md) (an agent in verticals, rules and an LLM together, a model per task), [Android](apps/android.md), [VASDK_PYTHON_APP](apps/python.md) (VASDK from Python: spoken sessions, tools and triggers in Python). Then the [examples](#worked-examples). |
| **Building from source** (prerequisites, CMake options, model downloads, air-gapped builds) | [Building VASDK](building.md). |
| **Using VASDK from another project** (CMake, `find_package`, pkg-config, packages) | [Using VASDK in your project](integrating.md). |
| **Getting the model files** (download, check, offline machines, licenses) | [Models](models.md). |
| **Choosing providers** (which STT/TTS/LLM, their options, models and licenses) | [Providers](provider-catalog.md), and how well each local one does on real speech: [Evaluation](evaluation.md). |
| **Implementing a provider** (new STT/TTS/VAD/LLM backend) | [Provider interfaces](reference/providers.md), especially the checklist at the end. |
| **Extending the core** (new processor, transport, frame type) | [Processors](reference/processors.md), [Transports](reference/transports.md), [Frames](reference/frames.md) - each ends with an extension guide. |
| **Doing compliance / legal review** | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). |
| **Working with an AI coding agent** (or wanting the checklists one would follow) | [AGENTS.md](../AGENTS.md): build commands, house rules, the definition of done, and the [skills](../skills/README.md), one per stage (VAD, STT, TTS, turn detection, wake word, LLM, NLU, audio, speaker id, transports) plus providers, core components, agents, apps and bindings. |

## Setup

- **[Building VASDK](building.md)** - prerequisites, presets, build options, dependencies, model files, air-gapped builds.
- **[Using VASDK in your project](integrating.md)** - the C library (`VASDK::c`) and the C++ targets, FetchContent, `find_package`, pkg-config, flavors, packages.
- **[Releasing VASDK](releasing.md)** - the version, the release workflow, what each channel publishes.
- **[Models](models.md)** - the model store: downloading, checking and finding the files local providers load; the `vasdk` command; licenses and gated models; offline machines.
- **[Providers](provider-catalog.md)** - every implementation, how the apps choose between them, per-provider options, sources and licenses.
- **[Evaluation](evaluation.md)** - the local providers measured on real speech (word error rate, VAD F1, turn accuracy, speaker EER, TTS intelligibility), the evaluation set, `vasdk_provider_eval`, and the baselines ctest holds them to.
- **[Provider pages](providers/README.md)** - one page per integration: platforms, options and defaults, models and licenses, usage, a flow diagram, tests.

## Core concepts (reference)

The five ideas, in reading order - each page states its source-of-truth headers, and the headers win whenever prose lags behind the code:

1. **[Frames](reference/frames.md)** - the typed data units flowing through the pipeline: the full catalog, priorities, flow directions, and how to add a type.
2. **[Processors](reference/processors.md)** - the pipeline stages: the `FrameProcessor` contract, the threading model, every built-in processor (including the VAD and agent state machines, with diagrams), and how to write your own.
3. **[Pipeline](reference/pipeline.md)** - chaining and lifecycle: `build()`/`start()`/`stop()`, ordering rules, how frames actually move.
4. **[Providers](reference/providers.md)** - the swappable AI/IO backends: all seven interfaces (audio source/sink, VAD, STT, TTS, DSP, turn detection), their contracts, and the new-provider checklist.
5. **[Transports](reference/transports.md)** - how frames enter and leave the process: local hardware, HTTP, WebSocket, the mock for tests, and the network-provider seam.

Built on top of those:

- **[The LLM stack](reference/llm.md)** - `LLMTransport`, prompt templates and chat formats, tools and dispatch, fire policies, streaming, cancellation.
- **[The goal layer](reference/goal.md)** - goal-oriented conversations: slots and validation, `GoalManager`, intent routing, persistence/resume, the conversation simulator.
- **[VAEngine](reference/engine.md)** - a complete assistant from an engine config: the standard pipeline, slots for your own processors, `say` / `injectUserText` / `interrupt`, events for transcripts, replies, state changes, tool calls and errors, and many conversations at once over shared models.
- **[Agents](reference/flows.md)** - scripted conversations with no LLM: pages, keyword intents, routes, conditions, stored values, forms, entities, shared commands and digressions in an agent file, checked and tested with `vasdk_agentc`. An agent can be split into parts (`include_files`), and with an LLM it hands what it doesn't answer itself to the LLM (`target: "LLM"`).
- **[Languages](reference/languages.md)** - language packs: the words for numbers, dates, yes and no, the built-in intents and back-channels, the engine's own replies and how values are said, written per language by whoever deploys the agent. VASDK ships en-US; an agent lists the languages its users speak.
- **[The C API](reference/c-api.md)** - agents and spoken sessions from any language: load an agent, talk to it by text or by voice, write its steps, tools and envelopes in C, give the LLM tools, fire triggers, and run its test cases. Python and Kotlin wrap it (`bindings/python`, `bindings/kotlin`).
- **[Kotlin](reference/kotlin.md)** - the Kotlin binding on the JVM and Android (an AAR): agents, sessions, every control, and providers and processors written in Kotlin.
- **[Engine config files](reference/engine-config.md)** - choosing VAD/STT/TTS providers and LLM settings in a text-proto file instead of code: formats, `${ENV}` secrets, provider ids and options, fallbacks, and [a different LLM for each task](reference/engine-config.md#different-llms-for-different-tasks).
- **[Context and metrics](reference/context-and-metrics.md)** - feeding situational context into prompts; instrumenting and exporting pipeline metrics.

## Example apps

Full guides to the five apps in the repository, with what each shows and how it's built: **[docs/apps](apps/README.md)**.

- [VASDK_Pipeline](apps/pipeline.md): the desktop voice loop, tools, metrics, browser calls, an agent before the LLM.
- [VASDK_CallCenter](apps/callcenter.md): a phone call answered by agent files: routing, forms, CRM tools, a handoff.
- [VASDK_Car](apps/car.md): ten verticals, each with its code, agent part, tests and panel; triggers from the car's sensors; stories, games and questions handed to the LLM; a model per task.
- [Android](apps/android.md): the full stack on a phone, device tools, background triggers.
- [VASDK_PYTHON_APP](apps/python.md): a kitchen assistant in Python: spoken sessions from Python, the agent's and the LLM's tools in Python, timers that ring over anything.

## Worked examples

- [A minimal pipeline](examples/minimal-pipeline.md) - the smallest useful program.
- [Giving the LLM a tool](examples/llm-tools.md) - define, register, and dispatch a tool.
- [Defining a goal in JSON](examples/goal-definitions.md) - slot types, conditions, loading.
- [Testing without a microphone](examples/testing-with-mock-transport.md) - `MockTransport` and the goal conversation simulator.
- [An agent in another language](examples/languages.md) - a language pack, an agent in French that understands English, tested with `vasdk_agentc`; packs from C++, C and Python, and a C++ language for grammar a pack can't express.

## Repository layout

```text
VASDK/                Core library: the pipeline runtime, all interfaces, the C API
VASDK_3P_IMPL/         Provider implementations, one CMake target each (see table below)
VASDK_APP/             General-purpose voice assistant showcase (VASDK_Pipeline)
VASDK_CALLCENTER_APP/  Goal-oriented call center showcase (VASDK_CallCenter)
VASDK_CAR_APP/         In-car assistant showcase with a live dashboard (VASDK_Car)
VASDK_ANDROID/         Android app + JNI bindings around the core library
VASDK_PYTHON_APP/      Kitchen assistant in Python, on the Python binding
VASDK/languages/       The built-in language pack (en-US): copy it to write another language
bindings/python/       The Python binding (the C API, over ctypes)
bindings/kotlin/       The Kotlin binding (the C API, over JNA): JVM, and Android's AAR
cmake/                 Packaging: the C library (vasdk_c), dependencies, flavors, find_package(VASDK)
tests/consumer/        A program built against an installed VASDK, as another project builds it
skills/                Task skills for coding agents and contributors (index: AGENTS.md)
docs/apps/             The example apps' guides
docs/examples/         Worked examples (pipelines, tools, goals, testing)
prometheus/, grafana/  docker-compose stack for local metrics visualization
```

## Conventions used across the docs

- Every reference page links the header it documents; **the header is the source of truth** - this project changes daily and prose can lag.
- Interfaces here grow by **adding optional virtuals with safe defaults**, so custom implementations keep compiling across updates; pages call out which members are optional.
- File paths in code snippets are relative to the repository root.
- **The root README only links.** It holds the pitch, the demo and the quick start; every detail lives in exactly one page here.
