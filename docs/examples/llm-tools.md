# Example: giving the LLM a tool

A `va::llm::Tool` is a name, a description, typed parameters, and a handler that receives JSON arguments and returns a JSON result. Register it with a `ToolDispatchProcessor` and it's advertised to the LLM on the next request automatically; when the model calls it, the dispatcher runs your handler and feeds the result back into the conversation.

## Defining and registering a tool

```cpp
#include <VASDK/llm/Tool.h>
#include <VASDK/llm/ToolDispatchProcessor.h>

va::llm::Tool getWeather("get_weather", "Look up current weather for a city");
getWeather.addParameter({"city", "City name", "string", true})
          .onCall([](const nlohmann::json& args) -> nlohmann::json {
              return {{"city", args.at("city")}, {"tempC", 21}, {"conditions", "clear"}};
          });

toolDispatcher->registerTool(getWeather);
```

`Parameter` is `{name, description, type, required}`, where `type` is a JSON type string (`"string"`, `"number"`, `"integer"`, `"boolean"`, `"object"`). Tools can be toggled at runtime with `setEnabled(false)` without unregistering them.

## How the call flows

1. The LLM's streamed response contains a tool invocation. `LLMProcessor` parses it into a `ToolCallFrame` carrying the tool name and serialized JSON arguments.
2. `ToolDispatchProcessor` matches the frame to a registered tool, runs its handler, and emits a `ToolResultFrame` with the handler's JSON result.
3. The result is appended to the conversation and the LLM continues its turn, now with the data in context.

Because the whole exchange is frames, tool calls work identically with cloud transports (OpenAI, Anthropic) and local GGUF models through llama.cpp.

## Local models and prompt rendering

API transports receive tools as structured JSON (`Tool::toJson()`). Local text-format models get them rendered into the prompt instead, via `Tool::toPromptDescription()` and `ToolDispatchProcessor::renderToolDescriptions(ChatFormat)`, which formats the catalog for the model's chat format. If you register tools and use a local model, make sure the system prompt includes the rendered descriptions; `VASDK_APP/main_pipeline.cpp` shows the wiring.

## Full reference

Both showcase apps register tools: [`VASDK_APP/main_pipeline.cpp`](../../VASDK_APP/main_pipeline.cpp) ships `get_weather` and `get_time` demo tools, and [`VASDK_CALLCENTER_APP/callcenter_pipeline.cpp`](../../VASDK_CALLCENTER_APP/callcenter_pipeline.cpp) exposes a `lookup_account` CRM tool alongside the goal-management tools that `GoalManager` registers for itself.
