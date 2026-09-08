# VASDK documentation

Start here and pick your path.

## By audience

| You are… | Start with |
| --- | --- |
| **Evaluating VASDK** (PM, project lead, architect doing a first pass) | [Plain-language overview](overview.md) - what it is, what you can build, maturity, licensing. No code. |
| **New to the codebase** (any level, including junior) | Root [README](../README.md) for the quick start, then [Core concepts](#core-concepts-reference) below in order: frames → processors → pipeline. The [minimal pipeline example](examples/minimal-pipeline.md) is the fastest way to a mental model. |
| **Building an app on VASDK** | The showcase app guides: [VASDK_Pipeline](../VASDK_APP/README.md) (general assistant), [VASDK_CallCenter](../VASDK_CALLCENTER_APP/README.md) (goal-oriented), [Android](../VASDK_ANDROID/README.md). Then the [examples](#worked-examples). |
| **Implementing a provider** (new STT/TTS/VAD/LLM backend) | [Provider interfaces](reference/providers.md), especially the checklist at the end. |
| **Extending the core** (new processor, transport, frame type) | [Processors](reference/processors.md), [Transports](reference/transports.md), [Frames](reference/frames.md) - each ends with an extension guide. |
| **Doing compliance / legal review** | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). |

## Core concepts (reference)

The five ideas, in reading order - each page states its source-of-truth headers, and the headers win whenever prose lags behind the code:

1. **[Frames](reference/frames.md)** - the typed data units flowing through the pipeline: the full catalog, priorities, flow directions, and how to add a type.
2. **[Processors](reference/processors.md)** - the pipeline stages: the `FrameProcessor` contract, the threading model, every built-in processor (including the VAD and agent state machines, with diagrams), and how to write your own.
3. **[Pipeline](reference/pipeline.md)** - chaining and lifecycle: `build()`/`start()`/`stop()`, ordering rules, how frames actually move.
4. **[Providers](reference/providers.md)** - the swappable AI/IO backends: all seven interfaces (audio source/sink, VAD, STT, TTS, DSP, turn detection), their contracts, and the new-provider checklist.
5. **[Transports](reference/transports.md)** - how frames enter and leave the process: local hardware, HTTP, WebSocket, the mock for tests, and the network-provider seam.

Built on top of those:

- **[The LLM stack](reference/llm.md)** - `LLMTransport`, prompt templates and chat formats, tools and dispatch, fire policies, streaming, cancellation.
- **[The goal layer](reference/goal.md)** - goal-oriented conversations: slots and validation, `GoalManager`, intent routing, persistence/resume, the conversation simulator, multi-session `SessionManager`.
- **[RequestFlow spec](reference/requestflow-spec.md)** - implementation specification for RequestFlow, a deterministic flow/node/route orchestration runtime on top of VASDK.
- **[RequestFlow backend & webhook auth spec](reference/requestflow-backend-spec.md)** - the wire contract and authentication for the backend services `callWebhook` talks to, with per-language (Spring Boot/Java, Python, Go) implementation guidance.
- **[Context and metrics](reference/context-and-metrics.md)** - feeding situational context into prompts; instrumenting and exporting pipeline metrics.

## Worked examples

- [A minimal pipeline](examples/minimal-pipeline.md) - the smallest useful program.
- [Giving the LLM a tool](examples/llm-tools.md) - define, register, and dispatch a tool.
- [Defining a goal in JSON](examples/goal-definitions.md) - slot types, conditions, loading.
- [Testing without a microphone](examples/testing-with-mock-transport.md) - `MockTransport` and the goal conversation simulator.

## Conventions used across the docs

- Every reference page links the header it documents; **the header is the source of truth** - this project changes daily and prose can lag.
- Interfaces here grow by **adding optional virtuals with safe defaults**, so custom implementations keep compiling across updates; pages call out which members are optional.
- File paths in code snippets are relative to the repository root.
