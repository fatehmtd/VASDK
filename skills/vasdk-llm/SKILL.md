---
name: vasdk-llm
description: The LLM in VASDK - configuring the llm block (OpenAI-compatible endpoints, Anthropic, llama.cpp on the device), a model per task, tools and tool dispatch, prompt templates, streaming and cancellation, and adding a new LLM API or local runtime (LLMTransport + Completion). Use when working on LLM replies, tools, prompts, latency, or a new model backend.
---

# LLM

**What it does:** answers what the agent hands over and holds the open conversation. Two seams:

- **`LLMTransport`** (`VASDK/include/VASDK/llm/LLMTransport.h`): the streamed conversation. `LLMProcessor` sends the user's turn, streams the reply to TTS, and `ToolDispatchProcessor` runs tool calls ([llm reference](../../docs/reference/llm.md)).
- **`Completion`** (`VASDK/include/VASDK/llm/Completion.h`): one-shot asks the agent makes (intent checks, entity extraction, handed-over turns), with a time limit.

| | |
| --- | --- |
| Processors | `VASDK/include/VASDK/llm/LLMProcessor.h`, `VASDK/include/VASDK/llm/ToolDispatchProcessor.h`, [notes](../../docs/reference/processors.md#llmprocessor) |
| Tools | `VASDK/include/VASDK/llm/Tool.h`, [example](../../docs/examples/llm-tools.md) |
| Prompts | `VASDK/include/VASDK/llm/PromptTemplate.h` (API formats in `ApiPromptTemplates.h`, local chat formats in `LocalPromptTemplates.h`) |
| Config | `llm { endpoint model api_key api effort history ... }`, [engine config](../../docs/reference/engine-config.md#llm), [a model per task](../../docs/reference/engine-config.md#different-llms-for-different-tasks) |

## Built in

| Backend | Where | Seams | How to pick |
| --- | --- | --- | --- |
| OpenAI-compatible (OpenAI, OpenRouter, LM Studio, vLLM, Ollama, llama.cpp server) | core: `VASDK/include/VASDK/llm/OpenAITransport.h` | transport + `OpenAICompletion` | `llm { api: OPENAI_COMPATIBLE endpoint model api_key }` |
| Anthropic Messages API | core: `VASDK/include/VASDK/llm/AnthropicTransport.h` | transport + `AnthropicCompletion` | `llm { api: ANTHROPIC api_key }` |
| llama.cpp in-process (GGUF) | `VASDK_3P_IMPL/LlamaCpp/` (`-DVASDK_BUILD_LLAMACPP=ON`); config id `llamacpp` | `LlamaCppTransport` + `LlamaCppCompletion` | `providers { llm { id: "llamacpp" } }` (options `LlamaCppOptions`; `answer_agent` for the agent's asks), or `VASDK_Pipeline --llm-model-path`, `--agent-llm-model-path` |

## Using it

```textproto
llm { api: ANTHROPIC api_key: "${ANTHROPIC_API_KEY}" effort: "low" }
```

`speculative_firing` starts the request on a partial transcript (needs `turn_taking { streaming_transcription: true }`); `tool_filler_phrases` cover tool latency; `failure_replies` and `offline_replies` are what the user hears when it fails or the network is down. Follow-ups (the user keeps talking after an early end of turn) cancel and resend as one message ([follow-ups](../../docs/reference/llm.md#follow-ups)).

## Contract pitfalls

- **Cancellation:** `cancel()` must stop the HTTP stream at once; barge-in and follow-ups depend on it (`vasdk_http_cancel`).
- **Streaming:** `parseStreamingData()` sees partial chunks; text must reach TTS sentence by sentence, not at the end (`StreamingReplyExtractor`).
- **Tools:** a tool call and its result go into history in the API's own format; parallel calls and a cancel mid-tool must not leave a dangling call (`vasdk_tool_dispatch_race`).
- Never log prompts or replies with user content at `info`; honour `security` masking.

## Adding one

- **A new HTTP API** (another vendor's wire format): this belongs to the core, as Anthropic does. Add an `LLMTransport` subclass and a `Completion` subclass in `VASDK/src/llm/` over `network::HttpInterface` (no new dependency), a value in `LlmConfig.Api` in `VASDK/proto/vasdk/engine/engine_config.proto`, and the choice in `VASDK/src/engine/Session.cpp` where `ANTHROPIC` is chosen. Template: `VASDK/src/llm/AnthropicTransport.cpp` and `AnthropicCompletion` in `VASDK/src/llm/Completion.cpp`. Then [vasdk-bindings](../vasdk-bindings/SKILL.md) for C, Python and Android, and [vasdk-core-component](../vasdk-core-component/SKILL.md) for its doc page.
- **A local runtime** (another inference library): a 3P integration under `VASDK_3P_IMPL/<Name>/` that implements both seams, following [vasdk-provider](../vasdk-provider/SKILL.md) (`va::thirdparty::llm`, `<Name>Options` with defaults, cross-platform or warn, its own page, licenses). Template: `VASDK_3P_IMPL/LlamaCpp/` (`va::thirdparty::llm::LlamaCppTransport` and `LlamaCppCompletion`). Its options are the core's `llm::LLMConfig`, the struct every LLM seam takes, so it adds no struct of its own.
- **A tool:** [the example](../../docs/examples/llm-tools.md); an app's tools are registered by the app, the SDK's own (device tools on Android) in their platform layer.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- Unit: in `VASDK/tests/`, fake the HTTP layer and check the request payload (messages, tools, effort), the parsing of a recorded streamed reply, cancel mid-stream, and a tool round trip. Existing ones to extend or copy: `vasdk_llm_stream_speech`, `vasdk_streaming_reply_extractor`, `vasdk_tool_dispatch_race`, `vasdk_http_cancel`, `vasdk_llm_assist`.
- Integration: a real call that skips without its key (`GTEST_SKIP()`), through a `Session` with an engine config. For a local runtime: `vasdk_llamacpp_completion` (`VASDK_3P_IMPL/LlamaCpp/llamacpp_completion_test.cpp`) skips without a GGUF file.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds; a new API value is documented in the engine-config `llm` table, and a local runtime meets the [vasdk-provider](../vasdk-provider/SKILL.md#done-when) checks.
