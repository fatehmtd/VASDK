# The C API

[`VASDK/c/vasdk.h`](../../VASDK/include/VASDK/c/vasdk.h) gives any language that can call C what C++ apps have for [agents](flows.md): load an agent, talk to it by text, write its steps, tools and envelopes, and run its test cases. Data crosses as JSON or serialized protos ([`vasdk/flow/*.proto`](../../VASDK/proto/vasdk/flow/)), never as C++ types, so bindings (Python, Rust, Go, Swift) stay thin.

```c
#include <VASDK/c/vasdk.h>

static void create_booking(const char* args_json, vasdk_result* result, void* user) {
    vasdk_result_set(result, "{\"reference\": \"B-42\"}");     /* or vasdk_result_fail(result, "why") */
}

vasdk_registry* registry = vasdk_registry_create();
vasdk_registry_add_tool(registry, "create_booking", "Books a service", create_booking, NULL);
char* error = NULL;
vasdk_agent* agent = vasdk_agent_load("agents/garage/agent.txtpb", registry, &error);
vasdk_conversation* conversation = vasdk_conversation_create(agent);
char* reply = vasdk_conversation_start(conversation);   /* {"text": "Hi, ...", "page": "welcome", ...} */
vasdk_free(reply);
reply = vasdk_conversation_say(conversation, "book an oil change");
vasdk_free(reply);
vasdk_conversation_free(conversation);
vasdk_agent_free(agent);
vasdk_registry_free(registry);
```

| Part | Functions |
|---|---|
| Registry | `vasdk_registry_add_step` (for `custom` steps, by their params type; params arrive as JSON), `vasdk_registry_add_tool` (for `call_tool`; set or fail its `vasdk_result`), `vasdk_registry_add_envelope` (`before` returns nonzero when it answered the turn, `after` runs on the reply). |
| Turn, inside a callback | `vasdk_turn_text`, `vasdk_turn_lookup("$session.tier")` (JSON), `vasdk_turn_say`, `vasdk_turn_goto`, and `vasdk_turn_apply` with a [`StepOutcome`](../../VASDK/proto/vasdk/flow/step_outcome.proto): values to set and unset, texts to say, a target. A key it may not write (anything but `$session.<declared>`, `$page.locals.<name>`, `$form.<slot>`) fails it, and nothing applies. |
| Agents | `vasdk_agent_load(path)`, or `vasdk_agent_parse(data, length, format)` from text proto, JSON or binary. |
| Conversations | `vasdk_conversation_start`, `_say`, `_no_input`, `_select_choice`, `_fire_trigger`, `_set_param` (a JSON value), `_lookup`. Each turn returns the reply as JSON: `text`, `segments`, `choices`, `matched`, `intent`, `event`, `expects_reply`, `ends_session`, `flow`, `page`, `tool_calls`, `triggers`. |
| Tests | `vasdk_run_test_file(agent, path, &report)`: a [`FlowTestCase`](flows.md#checking-and-testing-vasdk_agentc) file, 1 when it passed. |

Text VASDK returns as `char*` is the caller's: free it with `vasdk_free`. On failure a function sets its `char** error` (also freed with `vasdk_free`) and returns `NULL` or 0. Callbacks and their `user` pointer must outlive the agents loaded with the registry. A conversation is used from one thread at a time; registries and agents can be shared. [`vasdk_c_test.c`](../../VASDK/tests/vasdk_c_test.c) uses all of it from C, in `ctest`.

Not in the C API yet: audio sessions (`Session`, `VAEngine`) and their events. Those need providers, which come from the provider catalog, outside core.
