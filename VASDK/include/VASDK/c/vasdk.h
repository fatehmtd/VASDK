/*
 * VASDK's C API: agents, conversations with them, and steps, tools and envelopes written in C
 * (or any language that calls C: Python, Rust, Go, Swift). Data crosses as JSON or as
 * serialized protos (vasdk/flow/*.proto), never as C++ types.
 *
 *   vasdk_registry* registry = vasdk_registry_create();
 *   vasdk_registry_add_tool(registry, "create_booking", "Books a slot", create_booking, NULL);
 *   char* error = NULL;
 *   vasdk_agent* agent = vasdk_agent_load("agents/garage/agent.txtpb", registry, &error);
 *   vasdk_conversation* conversation = vasdk_conversation_create(agent);
 *   char* reply = vasdk_conversation_start(conversation);      // {"text": "Hi, ...", ...}
 *   vasdk_free(reply);
 *   reply = vasdk_conversation_say(conversation, "book an oil change");
 *   ...
 *   vasdk_conversation_free(conversation); vasdk_agent_free(agent); vasdk_registry_free(registry);
 *
 * Text VASDK returns as `char*` is the caller's: free it with vasdk_free. Functions that can fail
 * take `char** error`: on failure they set it to a message (free it with vasdk_free) and return
 * NULL or 0. A conversation is used from one thread at a time; registries and agents may be shared.
 */
#ifndef VASDK_C_VASDK_H
#define VASDK_C_VASDK_H

#include <stddef.h>

#include "../vasdk_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vasdk_format {
    VASDK_FORMAT_TEXT = 0,    /* text proto */
    VASDK_FORMAT_JSON = 1,    /* the proto's JSON mapping */
    VASDK_FORMAT_BINARY = 2   /* serialized proto */
} vasdk_format;

typedef struct vasdk_registry vasdk_registry;          /* steps, tools and envelopes written in C */
typedef struct vasdk_agent vasdk_agent;                /* an agent, checked and compiled */
typedef struct vasdk_conversation vasdk_conversation;  /* one conversation's place in an agent */
typedef struct vasdk_turn vasdk_turn;                  /* the turn a step or envelope runs in */
typedef struct vasdk_result vasdk_result;              /* what a tool returns */

VASDK_EXPORT void vasdk_free(void* text);
VASDK_EXPORT const char* vasdk_version(void);

/* ---- Registry: what agents can name, written in C. The callbacks and their `user` must
 *      outlive every agent loaded with the registry. ---- */

VASDK_EXPORT vasdk_registry* vasdk_registry_create(void);
VASDK_EXPORT void vasdk_registry_free(vasdk_registry* registry);

/* A step for `custom { [type.googleapis.com/<params_type>] { ... } }`: gets its params as JSON.
 * The params type must be known to the agent (proto_imports). */
typedef void (*vasdk_step_fn)(const char* params_json, vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_step(vasdk_registry* registry, const char* params_type, vasdk_step_fn step, void* user);

/* A tool for call_tool steps: gets the step's args as a JSON object, and sets its result
 * (vasdk_result_set) or fails it (vasdk_result_fail: tool-error). It may block. */
typedef void (*vasdk_tool_fn)(const char* args_json, vasdk_result* result, void* user);
VASDK_EXPORT int vasdk_registry_add_tool(vasdk_registry* registry, const char* name, const char* description,
                                         vasdk_tool_fn tool, void* user);
VASDK_EXPORT void vasdk_result_set(vasdk_result* result, const char* json);
VASDK_EXPORT void vasdk_result_fail(vasdk_result* result, const char* error);

/* An envelope for `envelopes: ["<name>"]`: `before` runs before the page's routes and returns
 * nonzero when it answered the turn; `after` runs once the reply is ready. Either may be NULL. */
typedef int (*vasdk_envelope_before_fn)(vasdk_turn* turn, void* user);
typedef void (*vasdk_envelope_after_fn)(vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_envelope(vasdk_registry* registry, const char* name, vasdk_envelope_before_fn before,
                                             vasdk_envelope_after_fn after, void* user);

/* ---- Turn: only inside a step's or envelope's callback. ---- */

/* What the user said this turn; valid until the callback returns. */
VASDK_EXPORT const char* vasdk_turn_text(vasdk_turn* turn);
/* The value a reference holds now ("$session.tier", "$form.day"), as JSON; "null" when unknown. */
VASDK_EXPORT char* vasdk_turn_lookup(vasdk_turn* turn, const char* reference);
VASDK_EXPORT void vasdk_turn_say(vasdk_turn* turn, const char* text);
VASDK_EXPORT void vasdk_turn_goto(vasdk_turn* turn, const char* target);
/* Applies a vasdk.flow.StepOutcome: values to set and unset, texts to say, a target. Keys other
 * than $session.<declared>, $page.locals.<name> and $form.<slot> fail it, and nothing applies. */
VASDK_EXPORT int vasdk_turn_apply(vasdk_turn* turn, const void* outcome, size_t length, vasdk_format format, char** error);

/* ---- Agents ---- */

/* An agent file (.txtpb, .json or .pb; its imports and goal files beside it). registry may be NULL. */
VASDK_EXPORT vasdk_agent* vasdk_agent_load(const char* path, const vasdk_registry* registry, char** error);
/* A vasdk.flow.Agent from memory. */
VASDK_EXPORT vasdk_agent* vasdk_agent_parse(const void* data, size_t length, vasdk_format format,
                                            const vasdk_registry* registry, char** error);
VASDK_EXPORT void vasdk_agent_free(vasdk_agent* agent);

/* ---- Conversations, by text. Each returns the reply as JSON:
 *   {"text", "segments": [{"text", "interruptible"}], "choices": [{"label", "value"}], "matched",
 *    "intent", "event", "expects_reply", "ends_session", "flow", "page",
 *    "tool_calls": [{"name", "arguments", "result", "ok"}], "triggers": [{"name", "decision"}]} ---- */

VASDK_EXPORT vasdk_conversation* vasdk_conversation_create(const vasdk_agent* agent);
VASDK_EXPORT void vasdk_conversation_free(vasdk_conversation* conversation);
VASDK_EXPORT char* vasdk_conversation_start(vasdk_conversation* conversation);
VASDK_EXPORT char* vasdk_conversation_say(vasdk_conversation* conversation, const char* text);
VASDK_EXPORT char* vasdk_conversation_no_input(vasdk_conversation* conversation);
VASDK_EXPORT char* vasdk_conversation_select_choice(vasdk_conversation* conversation, int index);
VASDK_EXPORT char* vasdk_conversation_fire_trigger(vasdk_conversation* conversation, const char* name, const char* payload_json);
/* Sets a declared session param from a JSON value ("\"gold\"", "3", "true"). */
VASDK_EXPORT int vasdk_conversation_set_param(vasdk_conversation* conversation, const char* name, const char* value_json,
                                              char** error);
/* The value a reference holds now, as JSON. */
VASDK_EXPORT char* vasdk_conversation_lookup(vasdk_conversation* conversation, const char* reference);

/* ---- Tests: plays a vasdk.flow.FlowTestCase file. 1 when it passed; `report` (may be NULL)
 *      gets what differed, one line each. ---- */
VASDK_EXPORT int vasdk_run_test_file(const vasdk_agent* agent, const char* path, char** report);

#ifdef __cplusplus
}
#endif

#endif /* VASDK_C_VASDK_H */
