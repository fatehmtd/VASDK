/*
 * VASDK's C API: agents, conversations with them, and steps, tools and envelopes written in C
 * (or any language that calls C: Python, Rust, Go, Swift). Data crosses as protos
 * (vasdk/flow/*.proto, vasdk/engine/*.proto), never as C++ types. The *_message functions (and
 * vasdk_conversation_send, vasdk_session_send) take and give a proto in the vasdk_format asked
 * for: binary for a language with protobuf. The others, for C, take and give JSON. Tools and
 * entity extractors take and give JSON either way: an LLM writes their arguments.
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

typedef struct vasdk_registry vasdk_registry;          /* steps, tools, envelopes, page types and entity extractors written in C */
typedef struct vasdk_agent vasdk_agent;                /* an agent, checked and compiled */
typedef struct vasdk_conversation vasdk_conversation;  /* one conversation's place in an agent */
typedef struct vasdk_turn vasdk_turn;                  /* the turn a step or envelope runs in */
typedef struct vasdk_result vasdk_result;              /* what a tool returns */

/* A proto VASDK gives: `length` bytes in the format asked for, valid until the callback returns. */
typedef void (*vasdk_message_fn)(const void* data, size_t length, void* user);

/* Frees the text and the messages VASDK returned. */
VASDK_EXPORT void vasdk_free(void* text);
VASDK_EXPORT const char* vasdk_version(void);
/* How much VASDK logs, to the console: "trace", "debug", "info" (the default), "warn", "error" or "off".
 * 0 for a level it doesn't know. */
VASDK_EXPORT int vasdk_set_log_level(const char* level);

/* ---- Registry: what agents can name, written in C. The callbacks and their `user` must
 *      outlive every agent loaded with the registry. ---- */

VASDK_EXPORT vasdk_registry* vasdk_registry_create(void);
VASDK_EXPORT void vasdk_registry_free(vasdk_registry* registry);

/* A step for `custom { [type.googleapis.com/<params_type>] { ... } }`: gets its params as JSON.
 * The params type must be known to the agent (proto_imports). */
typedef void (*vasdk_step_fn)(const char* params_json, vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_step(vasdk_registry* registry, const char* params_type, vasdk_step_fn step, void* user);
/* The same, its params as the message itself, in `format`. */
typedef void (*vasdk_step_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_step_message(vasdk_registry* registry, const char* params_type, vasdk_format format,
                                                 vasdk_step_message_fn step, void* user);

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

/* A page type for pages with `type: "<name>"`, their params (`params { [type.googleapis.com/<params_type>]
 * { ... } }`) as JSON. `on_enter` runs when the page is entered; `on_turn` gets each turn on the page and
 * returns nonzero when it answered it (zero: the page's routes get it). params_type may be NULL (the type
 * takes no params: "{}"), and either callback may be NULL. Replaces a page type of the same name.
 * Targets a C page type goes to aren't checked when the agent loads. */
typedef void (*vasdk_page_enter_fn)(const char* params_json, vasdk_turn* turn, void* user);
typedef int (*vasdk_page_turn_fn)(const char* params_json, vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_page_type(vasdk_registry* registry, const char* name, const char* params_type,
                                              vasdk_page_enter_fn on_enter, vasdk_page_turn_fn on_turn, void* user);
/* The same, its params as the message itself, in `format` (a type without params: 0 bytes). */
typedef void (*vasdk_page_enter_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
typedef int (*vasdk_page_turn_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
VASDK_EXPORT int vasdk_registry_add_page_type_message(vasdk_registry* registry, const char* name, const char* params_type,
                                                      vasdk_format format, vasdk_page_enter_message_fn on_enter,
                                                      vasdk_page_turn_message_fn on_turn, void* user);

/* Finds the values of `code { extractor: "<name>" }` entities in the user's words (a database, the
 * phone's contacts). Gets {"words": ["the", "user's", "words"], "text": "..."} and sets, with
 * vasdk_result_set, an array of what it found:
 *   [{"start": 2, "length": 1, "value": "mom-mobile", "spoken": "Mom", "attributes": {"label": "mobile"}}]
 * start and length index the words; spoken and attributes may be left out. Called on the
 * conversation's thread each time the entity is looked for: it should be quick. */
typedef void (*vasdk_entity_extractor_fn)(const char* request_json, vasdk_result* result, void* user);
VASDK_EXPORT int vasdk_registry_add_entity_extractor(vasdk_registry* registry, const char* name,
                                                     vasdk_entity_extractor_fn extractor, void* user);

/* ---- Turn: only inside a step's or envelope's callback. ---- */

/* What the user said this turn; valid until the callback returns. */
VASDK_EXPORT const char* vasdk_turn_text(vasdk_turn* turn);
/* The value a reference holds now ("$session.tier", "$form.day"), as JSON; "null" when unknown. */
VASDK_EXPORT char* vasdk_turn_lookup(vasdk_turn* turn, const char* reference);
/* The same, a google.protobuf.Value in `format`, *length bytes long. Free it with vasdk_free. */
VASDK_EXPORT void* vasdk_turn_lookup_message(vasdk_turn* turn, const char* reference, vasdk_format format, size_t* length);
VASDK_EXPORT void vasdk_turn_say(vasdk_turn* turn, const char* text);
VASDK_EXPORT void vasdk_turn_goto(vasdk_turn* turn, const char* target);
/* Applies a vasdk.flow.StepOutcome: values to set and unset, texts to say, a target. Keys other
 * than $session.<declared>, $page.locals.<name> and $form.<slot> fail it, and nothing applies. */
VASDK_EXPORT int vasdk_turn_apply(vasdk_turn* turn, const void* outcome, size_t length, vasdk_format format, char** error);

/* ---- Agents ---- */

/* An agent file (.txtpb, .json or .pb; its imports and goal files beside it). registry may be NULL. */
VASDK_EXPORT vasdk_agent* vasdk_agent_load(const char* path, const vasdk_registry* registry, char** error);
/* The .proto files of the agent's proto_imports and those they import, dependencies first, as a
 * serialized google.protobuf.FileDescriptorSet: for a language that builds its message classes at
 * run time, to read the agent's own types as messages. Its size in *length; empty without proto_imports. */
VASDK_EXPORT void* vasdk_agent_proto_files(const vasdk_agent* agent, size_t* length);
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

/* Every turn of a conversation, as protos (vasdk/flow/conversation.proto): `input`, a
 * vasdk.flow.ConversationInput in `format` (start, text, no_input, select_choice, fire_trigger),
 * gets the vasdk.flow.ConversationReply in the same format, *reply_length bytes long. Free it
 * with vasdk_free. NULL, with `error` set, for an input that doesn't parse or is empty. */
VASDK_EXPORT void* vasdk_conversation_send(vasdk_conversation* conversation, const void* input, size_t length,
                                           vasdk_format format, size_t* reply_length, char** error);
/* Sets a declared session param from a google.protobuf.Value in `format`. */
VASDK_EXPORT int vasdk_conversation_set_param_message(vasdk_conversation* conversation, const char* name, const void* value,
                                                      size_t length, vasdk_format format, char** error);
/* The value a reference holds now, a google.protobuf.Value in `format`. Free it with vasdk_free. */
VASDK_EXPORT void* vasdk_conversation_lookup_message(vasdk_conversation* conversation, const char* reference,
                                                     vasdk_format format, size_t* length);

/* ---- Tests: plays a vasdk.flow.FlowTestCase file. 1 when it passed; `report` (may be NULL)
 *      gets what differed, one line each. ---- */
VASDK_EXPORT int vasdk_run_test_file(const vasdk_agent* agent, const char* path, char** report);

/* ---- Sessions: a spoken conversation, with the speech providers an engine config names
 *      (VAD, STT, TTS, an LLM) and an agent. The app brings the audio: it pushes the user's
 *      16-bit mono PCM in, and gets the assistant's speech through a callback (a phone line,
 *      a file, its own audio device).
 *
 *   vasdk_providers* providers = vasdk_providers_create();
 *   vasdk_providers_add_all(providers);                 // ProviderCatalog: every provider built in
 *   vasdk_session* session = vasdk_session_create("engine.txtpb", providers, agent, NULL, &error);
 *   vasdk_session_on_event(session, on_event, NULL);    // {"agentReply": {"text": ...}}, ...
 *   vasdk_session_on_audio(session, play, NULL);
 *   vasdk_session_start(session, &error);
 *   vasdk_session_push_audio(session, pcm, samples);    // as the audio comes in
 *   ...
 *   vasdk_session_free(session); vasdk_providers_free(providers);
 * ---- */

typedef struct vasdk_providers vasdk_providers;   /* the providers a config can name, by id */
typedef struct vasdk_session vasdk_session;

/* Empty; vasdk_providers_add_all (ProviderCatalog) adds every provider built into the binary. */
VASDK_EXPORT vasdk_providers* vasdk_providers_create(void);
VASDK_EXPORT void vasdk_providers_free(vasdk_providers* providers);

/* Each event, a vasdk.engine.Event as JSON (vasdk/engine/events.proto), on the session's event thread. */
typedef void (*vasdk_event_fn)(const char* event_json, void* user);
/* The assistant's speech: `samples` 16-bit mono samples at vasdk_session_output_rate, on a pipeline thread. */
typedef void (*vasdk_audio_fn)(const short* pcm, size_t samples, void* user);

/* An engine config file (.txtpb, .json or .pb; ${ENV} expanded) and an agent (NULL: the LLM only).
 * options_json may be NULL: {"id": "call-1", "models_dir": "models", "system_prompt": "...", "format_instruction": "..."}
 * (the LLM's instructions, with {context} for vasdk_session_set_context's; how its replies are formatted).
 * The config's audio must be 16-bit PCM (the default). providers and agent must outlive it. */
VASDK_EXPORT vasdk_session* vasdk_session_create(const char* config_path, const vasdk_providers* providers,
                                                 const vasdk_agent* agent, const char* options_json, char** error);
/* The same, its options a vasdk.engine.SessionOptions (vasdk/engine/session.proto) in `format`; options may be NULL. */
VASDK_EXPORT vasdk_session* vasdk_session_create_message(const char* config_path, const vasdk_providers* providers,
                                                         const vasdk_agent* agent, const void* options, size_t length,
                                                         vasdk_format format, char** error);
VASDK_EXPORT void vasdk_session_free(vasdk_session* session);   /* stops it first */
/* Before vasdk_session_start. */
VASDK_EXPORT void vasdk_session_on_event(vasdk_session* session, vasdk_event_fn on_event, void* user);
/* Each event as the vasdk.engine.Event itself, in `format`: instead of vasdk_session_on_event. */
VASDK_EXPORT void vasdk_session_on_event_message(vasdk_session* session, vasdk_message_fn on_event, vasdk_format format,
                                                 void* user);
VASDK_EXPORT void vasdk_session_on_audio(vasdk_session* session, vasdk_audio_fn on_audio, void* user);
/* The agent's greeting picks up where a vasdk_session_snapshot left it. */
VASDK_EXPORT void vasdk_session_resume(vasdk_session* session, const char* snapshot_json);
/* The same, from a vasdk.flow.ConversationSnapshot (vasdk/flow/snapshot.proto) in `format`. 0, with *error set, when it doesn't parse. */
VASDK_EXPORT int vasdk_session_resume_message(vasdk_session* session, const void* snapshot, size_t length, vasdk_format format,
                                              char** error);
VASDK_EXPORT int vasdk_session_start(vasdk_session* session, char** error);
VASDK_EXPORT void vasdk_session_stop(vasdk_session* session);
/* Sample rates, in Hz, of the audio pushed in and given out (the config's audio, default 16000). */
VASDK_EXPORT int vasdk_session_input_rate(const vasdk_session* session);
VASDK_EXPORT int vasdk_session_output_rate(const vasdk_session* session);
/* The user's audio, as it comes: any number of samples. Thread-safe. */
VASDK_EXPORT void vasdk_session_push_audio(vasdk_session* session, const short* pcm, size_t samples);
/* A user turn, as if spoken; words the assistant says. Thread-safe. */
VASDK_EXPORT void vasdk_session_inject_text(vasdk_session* session, const char* text);
VASDK_EXPORT void vasdk_session_say(vasdk_session* session, const char* text);
/* Sets a declared agent session param from a JSON value. Thread-safe. */
VASDK_EXPORT int vasdk_session_set_param(vasdk_session* session, const char* name, const char* value_json, char** error);
/* Does what a vasdk.engine.Command in `format` asks (vasdk/engine/events.proto): inject_text,
 * select_choice, fire_trigger, say, interrupt, set_session_param. 0, with `error` set, for one
 * that doesn't parse or is empty. Thread-safe. */
VASDK_EXPORT int vasdk_session_send(vasdk_session* session, const void* command, size_t length, vasdk_format format,
                                    char** error);
/* Nonzero: the network is up. Down: local fallbacks speak and listen, and the LLM rests. Thread-safe. */
VASDK_EXPORT void vasdk_session_set_network_available(vasdk_session* session, int available);
/* Where the agent's conversation is, to resume it later ("" when there's nothing to resume): the
 * JSON form of a vasdk.flow.ConversationSnapshot. */
VASDK_EXPORT char* vasdk_session_snapshot(vasdk_session* session);
/* The same, the ConversationSnapshot itself in `format` (its flow empty when there's nothing to resume); its size in *length. */
VASDK_EXPORT void* vasdk_session_snapshot_message(vasdk_session* session, vasdk_format format, size_t* length);

/* A tool the session's LLM may call, written by the app. parameters_json (may be NULL) is a JSON
 * array of {"name", "description", "type": "string" | "number" | "integer" | "boolean", "required"}.
 * The callback gets the LLM's arguments as JSON, and sets the result (vasdk_result_set) or fails
 * it (vasdk_result_fail: the LLM is told the error). It runs on a pipeline thread and may block.
 * Before vasdk_session_start. For an agent's call_tool steps, register tools with the registry. */
VASDK_EXPORT int vasdk_session_add_tool(vasdk_session* session, const char* name, const char* description,
                                        const char* parameters_json, vasdk_tool_fn tool, void* user, char** error);
/* The same, declared by a vasdk.engine.ToolDeclaration (vasdk/engine/session.proto) in `format`. */
VASDK_EXPORT int vasdk_session_add_tool_message(vasdk_session* session, const void* declaration, size_t length,
                                                vasdk_format format, vasdk_tool_fn tool, void* user, char** error);
/* Fires one of the agent's app_event triggers by its event name; payload_json (a JSON object, may
 * be NULL) is $trigger.payload. Its delivery decides when it's said. Thread-safe. */
VASDK_EXPORT int vasdk_session_fire_trigger(vasdk_session* session, const char* name, const char* payload_json, char** error);
/* The app's state, a JSON object: what the agent's $context.* reads (and its context_change
 * triggers watch), and {context} in the system prompt. Each top-level key replaces the one before. Thread-safe. */
VASDK_EXPORT int vasdk_session_set_context(vasdk_session* session, const char* context_json, char** error);
/* The same, from a google.protobuf.Struct in `format`. */
VASDK_EXPORT int vasdk_session_set_context_message(vasdk_session* session, const void* context, size_t length,
                                                   vasdk_format format, char** error);

#ifdef __cplusplus
}
#endif

#endif /* VASDK_C_VASDK_H */
