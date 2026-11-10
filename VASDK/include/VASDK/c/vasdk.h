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

#include "vasdk_c_export.h"

/* The C API's binary interface: a release raises it only when a function changes or goes away,
 * which never happens within a major version (docs/reference/c-api.md, "Stability"). The C
 * library's soname is this number: libvasdk_c.so.1. */
#define VASDK_C_API_VERSION 1

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
VASDK_C_API void vasdk_free(void* text);
VASDK_C_API const char* vasdk_version(void);
/* The library's VASDK_C_API_VERSION: a program built against a header with a higher one needs a
 * newer library (compare it at start-up when the library is loaded at run time). */
VASDK_C_API int vasdk_c_api_version(void);
/* How much VASDK logs, to the console: "trace", "debug", "info" (the default), "warn", "error" or "off".
 * 0 for a level it doesn't know. */
VASDK_C_API int vasdk_set_log_level(const char* level);

/* ---- Registry: what agents can name, written in C. The callbacks and their `user` must
 *      outlive every agent loaded with the registry. ---- */

VASDK_C_API vasdk_registry* vasdk_registry_create(void);
VASDK_C_API void vasdk_registry_free(vasdk_registry* registry);

/* A step for `custom { [type.googleapis.com/<params_type>] { ... } }`: gets its params as JSON.
 * The params type must be known to the agent (proto_imports). */
typedef void (*vasdk_step_fn)(const char* params_json, vasdk_turn* turn, void* user);
VASDK_C_API int vasdk_registry_add_step(vasdk_registry* registry, const char* params_type, vasdk_step_fn step, void* user);
/* The same, its params as the message itself, in `format`. */
typedef void (*vasdk_step_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
VASDK_C_API int vasdk_registry_add_step_message(vasdk_registry* registry, const char* params_type, vasdk_format format,
                                                 vasdk_step_message_fn step, void* user);

/* A tool for call_tool steps: gets the step's args as a JSON object, and sets its result
 * (vasdk_result_set) or fails it (vasdk_result_fail: tool-error). It may block. */
typedef void (*vasdk_tool_fn)(const char* args_json, vasdk_result* result, void* user);
VASDK_C_API int vasdk_registry_add_tool(vasdk_registry* registry, const char* name, const char* description,
                                         vasdk_tool_fn tool, void* user);
VASDK_C_API void vasdk_result_set(vasdk_result* result, const char* json);
VASDK_C_API void vasdk_result_fail(vasdk_result* result, const char* error);
/* Sets a vector of `count` floats: what a speaker or sentence encoder provider gives. */
VASDK_C_API void vasdk_result_set_floats(vasdk_result* result, const float* values, size_t count);

/* An envelope for `envelopes: ["<name>"]`: `before` runs before the page's routes and returns
 * nonzero when it answered the turn; `after` runs once the reply is ready. Either may be NULL. */
typedef int (*vasdk_envelope_before_fn)(vasdk_turn* turn, void* user);
typedef void (*vasdk_envelope_after_fn)(vasdk_turn* turn, void* user);
VASDK_C_API int vasdk_registry_add_envelope(vasdk_registry* registry, const char* name, vasdk_envelope_before_fn before,
                                             vasdk_envelope_after_fn after, void* user);

/* A page type for pages with `type: "<name>"`, their params (`params { [type.googleapis.com/<params_type>]
 * { ... } }`) as JSON. `on_enter` runs when the page is entered; `on_turn` gets each turn on the page and
 * returns nonzero when it answered it (zero: the page's routes get it). params_type may be NULL (the type
 * takes no params: "{}"), and either callback may be NULL. Replaces a page type of the same name.
 * Targets a C page type goes to aren't checked when the agent loads. */
typedef void (*vasdk_page_enter_fn)(const char* params_json, vasdk_turn* turn, void* user);
typedef int (*vasdk_page_turn_fn)(const char* params_json, vasdk_turn* turn, void* user);
VASDK_C_API int vasdk_registry_add_page_type(vasdk_registry* registry, const char* name, const char* params_type,
                                              vasdk_page_enter_fn on_enter, vasdk_page_turn_fn on_turn, void* user);
/* The same, its params as the message itself, in `format` (a type without params: 0 bytes). */
typedef void (*vasdk_page_enter_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
typedef int (*vasdk_page_turn_message_fn)(const void* params, size_t length, vasdk_turn* turn, void* user);
VASDK_C_API int vasdk_registry_add_page_type_message(vasdk_registry* registry, const char* name, const char* params_type,
                                                      vasdk_format format, vasdk_page_enter_message_fn on_enter,
                                                      vasdk_page_turn_message_fn on_turn, void* user);

/* Finds the values of `code { extractor: "<name>" }` entities in the user's words (a database, the
 * phone's contacts). Gets {"words": ["the", "user's", "words"], "text": "..."} and sets, with
 * vasdk_result_set, an array of what it found:
 *   [{"start": 2, "length": 1, "value": "mom-mobile", "spoken": "Mom", "attributes": {"label": "mobile"}}]
 * start and length index the words; spoken and attributes may be left out. Called on the
 * conversation's thread each time the entity is looked for: it should be quick. */
typedef void (*vasdk_entity_extractor_fn)(const char* request_json, vasdk_result* result, void* user);
VASDK_C_API int vasdk_registry_add_entity_extractor(vasdk_registry* registry, const char* name,
                                                     vasdk_entity_extractor_fn extractor, void* user);

/* A language agents may name in locale or languages: a vasdk.nlu.Language pack
 * (vasdk/nlu/language.proto) in `format`, `length` bytes; one of the same code replaces it. 1 when
 * it checks; 0, with *error set, when it doesn't. A pack the agent lists in language_files needs
 * no call. vasdk_language_builtin gives VASDK's own en-US pack, to start one from. */
VASDK_C_API int vasdk_registry_add_language(vasdk_registry* registry, const void* pack, size_t length, vasdk_format format,
                                             char** error);
/* VASDK's built-in en-US pack in `format`, *length bytes long. Free it with vasdk_free. */
VASDK_C_API void* vasdk_language_builtin(vasdk_format format, size_t* length);

/* ---- Turn: only inside a step's or envelope's callback. ---- */

/* What the user said this turn; valid until the callback returns. */
VASDK_C_API const char* vasdk_turn_text(vasdk_turn* turn);
/* The value a reference holds now ("$session.tier", "$form.day"), as JSON; "null" when unknown. */
VASDK_C_API char* vasdk_turn_lookup(vasdk_turn* turn, const char* reference);
/* The same, a google.protobuf.Value in `format`, *length bytes long. Free it with vasdk_free. */
VASDK_C_API void* vasdk_turn_lookup_message(vasdk_turn* turn, const char* reference, vasdk_format format, size_t* length);
VASDK_C_API void vasdk_turn_say(vasdk_turn* turn, const char* text);
VASDK_C_API void vasdk_turn_goto(vasdk_turn* turn, const char* target);
/* Applies a vasdk.flow.StepOutcome: values to set and unset, texts to say, a question to ask, a
 * target. Keys other than $session.<declared>, $page.locals.<name> and $form.<slot> fail it, and
 * nothing applies. */
VASDK_C_API int vasdk_turn_apply(vasdk_turn* turn, const void* outcome, size_t length, vasdk_format format, char** error);
/* Changes what the rest of the turn reads as the user's words (an envelope's before: redact them). */
VASDK_C_API void vasdk_turn_set_text(vasdk_turn* turn, const char* text);
/* A template with its references filled in ("Welcome back, $session.name."), as a say does. Free it with vasdk_free. */
VASDK_C_API char* vasdk_turn_render(vasdk_turn* turn, const char* text);
/* What the reply says so far; valid until the callback returns. */
VASDK_C_API const char* vasdk_turn_reply_text(vasdk_turn* turn);
/* The flow and the page the turn is on. Free them with vasdk_free. */
VASDK_C_API char* vasdk_turn_flow(vasdk_turn* turn);
VASDK_C_API char* vasdk_turn_page(vasdk_turn* turn);

/* ---- Agents ---- */

/* An agent file (.txtpb, .json or .pb; its imports and goal files beside it). registry may be NULL. */
VASDK_C_API vasdk_agent* vasdk_agent_load(const char* path, const vasdk_registry* registry, char** error);
/* The .proto files of the agent's proto_imports and those they import, dependencies first, as a
 * serialized google.protobuf.FileDescriptorSet: for a language that builds its message classes at
 * run time, to read the agent's own types as messages. Its size in *length; empty without proto_imports. */
VASDK_C_API void* vasdk_agent_proto_files(const vasdk_agent* agent, size_t* length);
/* A vasdk.flow.Agent from memory. */
VASDK_C_API vasdk_agent* vasdk_agent_parse(const void* data, size_t length, vasdk_format format,
                                            const vasdk_registry* registry, char** error);
VASDK_C_API void vasdk_agent_free(vasdk_agent* agent);

/* ---- Conversations, by text. Each returns the reply as JSON:
 *   {"text", "segments": [{"text", "interruptible"}], "choices": [{"label", "value"}], "matched",
 *    "intent", "event", "expects_reply", "ends_session", "flow", "page",
 *    "tool_calls": [{"name", "arguments", "result", "ok"}], "triggers": [{"name", "decision"}]} ---- */

VASDK_C_API vasdk_conversation* vasdk_conversation_create(const vasdk_agent* agent);
VASDK_C_API void vasdk_conversation_free(vasdk_conversation* conversation);
VASDK_C_API char* vasdk_conversation_start(vasdk_conversation* conversation);
VASDK_C_API char* vasdk_conversation_say(vasdk_conversation* conversation, const char* text);
VASDK_C_API char* vasdk_conversation_no_input(vasdk_conversation* conversation);
VASDK_C_API char* vasdk_conversation_select_choice(vasdk_conversation* conversation, int index);
VASDK_C_API char* vasdk_conversation_fire_trigger(vasdk_conversation* conversation, const char* name, const char* payload_json);
/* Sets a declared session param from a JSON value ("\"gold\"", "3", "true"). */
VASDK_C_API int vasdk_conversation_set_param(vasdk_conversation* conversation, const char* name, const char* value_json,
                                              char** error);
/* The value a reference holds now, as JSON. */
VASDK_C_API char* vasdk_conversation_lookup(vasdk_conversation* conversation, const char* reference);

/* Every turn of a conversation, as protos (vasdk/flow/conversation.proto): `input`, a
 * vasdk.flow.ConversationInput in `format` (start, text, no_input, select_choice, fire_trigger,
 * check_triggers, deliver_queued),
 * gets the vasdk.flow.ConversationReply in the same format, *reply_length bytes long. Free it
 * with vasdk_free. NULL, with `error` set, for an input that doesn't parse or is empty. */
VASDK_C_API void* vasdk_conversation_send(vasdk_conversation* conversation, const void* input, size_t length,
                                           vasdk_format format, size_t* reply_length, char** error);
/* Sets a declared session param from a google.protobuf.Value in `format`. */
VASDK_C_API int vasdk_conversation_set_param_message(vasdk_conversation* conversation, const char* name, const void* value,
                                                      size_t length, vasdk_format format, char** error);
/* The value a reference holds now, a google.protobuf.Value in `format`. Free it with vasdk_free. */
VASDK_C_API void* vasdk_conversation_lookup_message(vasdk_conversation* conversation, const char* reference,
                                                     vasdk_format format, size_t* length);
/* Where the conversation is, to pick it up later: a vasdk.flow.ConversationSnapshot in `format`
 * (sensitive slots and params left out; its flow empty when not started or ended). Free it with vasdk_free. */
VASDK_C_API void* vasdk_conversation_snapshot_message(vasdk_conversation* conversation, vasdk_format format, size_t* length);
/* Starts the conversation where a snapshot left it, instead of start: gives the ConversationReply
 * in `format`, *reply_length bytes. *resumed (may be NULL): 0 when the snapshot didn't fit the agent
 * and the conversation started over. NULL, with `error` set, for a snapshot that doesn't parse. */
VASDK_C_API void* vasdk_conversation_resume_message(vasdk_conversation* conversation, const void* snapshot, size_t length,
                                                     vasdk_format format, size_t* reply_length, int* resumed, char** error);
/* Replaces a dynamic entity's values for this conversation: a vasdk.engine.EntityValuesCommand
 * (vasdk/engine/events.proto: name, values) in `format`. 0, with `error` set, for an entity that isn't a dynamic list. */
VASDK_C_API int vasdk_conversation_set_entity_values_message(vasdk_conversation* conversation, const void* values,
                                                              size_t length, vasdk_format format, char** error);
/* The app's state, a google.protobuf.Struct in `format`: what $context.* reads. Each top-level key
 * replaces the one before. */
VASDK_C_API int vasdk_conversation_set_context_message(vasdk_conversation* conversation, const void* context, size_t length,
                                                        vasdk_format format, char** error);
/* The user cut the last reply off: a read-back cut off isn't confirmed by a plain "yes". */
VASDK_C_API void vasdk_conversation_reply_interrupted(vasdk_conversation* conversation);
/* How long to wait for an answer before a no_input, from the page's settings; 0: never. */
VASDK_C_API int vasdk_conversation_no_input_ms(vasdk_conversation* conversation);
/* When the next schedule trigger is due, in milliseconds since the epoch; -1 for none. */
VASDK_C_API long long vasdk_conversation_next_scheduled_trigger_ms(vasdk_conversation* conversation);
/* What the triggers have seen, as JSON, to keep reminders across restarts; free it with vasdk_free.
 * restore: 0 for text it can't read. */
VASDK_C_API char* vasdk_conversation_trigger_state(vasdk_conversation* conversation);
VASDK_C_API int vasdk_conversation_restore_trigger_state(vasdk_conversation* conversation, const char* state_json);

/* ---- Tests: plays a vasdk.flow.FlowTestCase file. 1 when it passed; `report` (may be NULL)
 *      gets what differed, one line each. ---- */
VASDK_C_API int vasdk_run_test_file(const vasdk_agent* agent, const char* path, char** report);
/* The same, a vasdk.flow.FlowTestCase (vasdk/flow/test_case.proto) from memory, in `format`. */
VASDK_C_API int vasdk_run_test_message(const vasdk_agent* agent, const void* test_case, size_t length, vasdk_format format,
                                        char** report);

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
VASDK_C_API vasdk_providers* vasdk_providers_create(void);
VASDK_C_API void vasdk_providers_free(vasdk_providers* providers);

/* Each event, a vasdk.engine.Event as JSON (vasdk/engine/events.proto), on the session's event thread. */
typedef void (*vasdk_event_fn)(const char* event_json, void* user);
/* The assistant's speech: `samples` 16-bit mono samples at vasdk_session_output_rate, on a pipeline thread. */
typedef void (*vasdk_audio_fn)(const short* pcm, size_t samples, void* user);

/* An engine config file (.txtpb, .json or .pb; ${ENV} expanded) and an agent (NULL: the LLM only).
 * options_json may be NULL: {"id": "call-1", "models_dir": "models", "system_prompt": "...", "format_instruction": "..."}
 * (the LLM's instructions, with {context} for vasdk_session_set_context's; how its replies are formatted).
 * The config's audio must be 16-bit PCM (the default). providers and agent must outlive it.
 * A config whose providers name a webrtc peer talks to a browser instead: the pushed audio and
 * the audio callback then go unused, and the page shows the session's state. */
VASDK_C_API vasdk_session* vasdk_session_create(const char* config_path, const vasdk_providers* providers,
                                                 const vasdk_agent* agent, const char* options_json, char** error);
/* The same, its options a vasdk.engine.SessionOptions (vasdk/engine/session.proto) in `format`; options may be NULL. */
VASDK_C_API vasdk_session* vasdk_session_create_message(const char* config_path, const vasdk_providers* providers,
                                                         const vasdk_agent* agent, const void* options, size_t length,
                                                         vasdk_format format, char** error);
/* The same, from an engine config the app built: a vasdk.engine.EngineConfig (vasdk/engine/engine_config.proto)
 * in `config_format`, its providers' options their own messages (vasdk/providers/options.proto). Unlike a
 * config file, nothing in it is expanded (${ENV}): it holds its values already. */
VASDK_C_API vasdk_session* vasdk_session_create_config(const void* config, size_t config_length, vasdk_format config_format,
                                                        const vasdk_providers* providers, const vasdk_agent* agent,
                                                        const void* options, size_t length, vasdk_format format, char** error);
VASDK_C_API void vasdk_session_free(vasdk_session* session);   /* stops it first */
/* Before vasdk_session_start. */
VASDK_C_API void vasdk_session_on_event(vasdk_session* session, vasdk_event_fn on_event, void* user);
/* Each event as the vasdk.engine.Event itself, in `format`: instead of vasdk_session_on_event. */
VASDK_C_API void vasdk_session_on_event_message(vasdk_session* session, vasdk_message_fn on_event, vasdk_format format,
                                                 void* user);
VASDK_C_API void vasdk_session_on_audio(vasdk_session* session, vasdk_audio_fn on_audio, void* user);
/* The agent's greeting picks up where a vasdk_session_snapshot left it. */
VASDK_C_API void vasdk_session_resume(vasdk_session* session, const char* snapshot_json);
/* The same, from a vasdk.flow.ConversationSnapshot (vasdk/flow/snapshot.proto) in `format`. 0, with *error set, when it doesn't parse. */
VASDK_C_API int vasdk_session_resume_message(vasdk_session* session, const void* snapshot, size_t length, vasdk_format format,
                                              char** error);
VASDK_C_API int vasdk_session_start(vasdk_session* session, char** error);
VASDK_C_API void vasdk_session_stop(vasdk_session* session);
/* Sample rates, in Hz, of the audio pushed in and given out (the config's audio, default 16000). */
VASDK_C_API int vasdk_session_input_rate(const vasdk_session* session);
VASDK_C_API int vasdk_session_output_rate(const vasdk_session* session);
/* The user's audio, as it comes: any number of samples. Thread-safe. */
VASDK_C_API void vasdk_session_push_audio(vasdk_session* session, const short* pcm, size_t samples);
/* A user turn, as if spoken; words the assistant says. Thread-safe. */
VASDK_C_API void vasdk_session_inject_text(vasdk_session* session, const char* text);
VASDK_C_API void vasdk_session_say(vasdk_session* session, const char* text);
/* Sets a declared agent session param from a JSON value. Thread-safe. */
VASDK_C_API int vasdk_session_set_param(vasdk_session* session, const char* name, const char* value_json, char** error);
/* Does what a vasdk.engine.Command in `format` asks (vasdk/engine/events.proto): inject_text,
 * select_choice, fire_trigger, say, interrupt, set_session_param, reset_conversation,
 * push_to_talk, follow_up_listen_ms, set_entity_values, set_backchannel, enroll_speaker,
 * add_speaker. 0, with `error` set, for one that doesn't parse, is empty, or the session can't do
 * (enroll_speaker without providers.speaker_embedding). Thread-safe. */
VASDK_C_API int vasdk_session_send(vasdk_session* session, const void* command, size_t length, vasdk_format format,
                                    char** error);
/* Nonzero: the network is up. Down: local fallbacks speak and listen, and the LLM rests. Thread-safe. */
VASDK_C_API void vasdk_session_set_network_available(vasdk_session* session, int available);
/* Where the agent's conversation is, to resume it later ("" when there's nothing to resume): the
 * JSON form of a vasdk.flow.ConversationSnapshot. */
VASDK_C_API char* vasdk_session_snapshot(vasdk_session* session);
/* The same, the ConversationSnapshot itself in `format` (its flow empty when there's nothing to resume); its size in *length. */
VASDK_C_API void* vasdk_session_snapshot_message(vasdk_session* session, vasdk_format format, size_t* length);

/* A tool the session's LLM may call, written by the app. parameters_json (may be NULL) is a JSON
 * array of {"name", "description", "type": "string" | "number" | "integer" | "boolean", "required"}.
 * The callback gets the LLM's arguments as JSON, and sets the result (vasdk_result_set) or fails
 * it (vasdk_result_fail: the LLM is told the error). It runs on a pipeline thread and may block.
 * Before vasdk_session_start. For an agent's call_tool steps, register tools with the registry. */
VASDK_C_API int vasdk_session_add_tool(vasdk_session* session, const char* name, const char* description,
                                        const char* parameters_json, vasdk_tool_fn tool, void* user, char** error);
/* The same, declared by a vasdk.engine.ToolDeclaration (vasdk/engine/session.proto) in `format`. */
VASDK_C_API int vasdk_session_add_tool_message(vasdk_session* session, const void* declaration, size_t length,
                                                vasdk_format format, vasdk_tool_fn tool, void* user, char** error);
/* Fires one of the agent's app_event triggers by its event name; payload_json (a JSON object, may
 * be NULL) is $trigger.payload. Its delivery decides when it's said. Thread-safe. */
VASDK_C_API int vasdk_session_fire_trigger(vasdk_session* session, const char* name, const char* payload_json, char** error);
/* The app's state, a JSON object: what the agent's $context.* reads (and its context_change
 * triggers watch), and {context} in the system prompt. Each top-level key replaces the one before. Thread-safe. */
VASDK_C_API int vasdk_session_set_context(vasdk_session* session, const char* context_json, char** error);
/* The same, from a google.protobuf.Struct in `format`. */
VASDK_C_API int vasdk_session_set_context_message(vasdk_session* session, const void* context, size_t length,
                                                   vasdk_format format, char** error);
/* The app's state, asked for each time the agent ($context.*, context_change triggers) or the LLM
 * ({context}) reads it: the callback sets a JSON object with vasdk_result_set, whose keys win over
 * set_context's. On the conversation's and the LLM's threads: keep it quick. Before vasdk_session_start. */
typedef void (*vasdk_context_fn)(vasdk_result* context, void* user);
VASDK_C_API void vasdk_session_on_context(vasdk_session* session, vasdk_context_fn on_context, void* user);

/* What the session runs: its providers, its LLM, its stages. Free it with vasdk_free. */
VASDK_C_API char* vasdk_session_describe(vasdk_session* session);
/* Nonzero: the network is down and the local fallbacks speak and listen. */
VASDK_C_API int vasdk_session_using_fallbacks(vasdk_session* session);
/* The enrolled speakers' voice prints (providers.speaker_embedding), a vasdk.engine.VoicePrints in
 * `format`, to keep and give back with an add_speaker command; its size in *length. Free it with vasdk_free. */
VASDK_C_API void* vasdk_session_speakers_message(vasdk_session* session, vasdk_format format, size_t* length);
/* When the agent's next schedule trigger is due, in milliseconds since the epoch, for an app that
 * must wake up for it (Android's AlarmManager); -1 for none. */
VASDK_C_API long long vasdk_session_next_scheduled_trigger_ms(vasdk_session* session);
/* What the agent's triggers have seen, as JSON: save it to keep reminders across restarts. Free it with vasdk_free. */
VASDK_C_API char* vasdk_session_trigger_state(vasdk_session* session);
VASDK_C_API void vasdk_session_restore_trigger_state(vasdk_session* session, const char* state_json);

/* ---- Processors written in the app, at a slot of the session's pipeline (engine/Session.h), in
 *      the order added. Before vasdk_session_start. 0 for a slot the kind doesn't take, or once started. ---- */
typedef enum vasdk_slot {
    VASDK_SLOT_AFTER_INPUT = 0,          /* the microphone's audio, before VAD */
    VASDK_SLOT_AFTER_TRANSCRIPTION = 1,  /* the user's words, before the agent */
    VASDK_SLOT_BEFORE_SYNTHESIS = 2,     /* the reply's words, before TTS */
    VASDK_SLOT_BEFORE_OUTPUT = 3         /* the reply's audio, before it plays */
} vasdk_slot;
/* Audio as it passes, 16-bit mono at the slot's rate. AFTER_INPUT: the microphone's, to read (a
 * recorder, a level meter). BEFORE_OUTPUT: the reply's, which the callback may change in place. On a pipeline thread. */
typedef void (*vasdk_audio_processor_fn)(short* pcm, size_t samples, int sample_rate, void* user);
VASDK_C_API int vasdk_session_add_audio_processor(vasdk_session* session, vasdk_slot slot, vasdk_audio_processor_fn processor,
                                                   void* user);
/* Text as it passes: AFTER_TRANSCRIPTION each final transcript, BEFORE_SYNTHESIS each part of a
 * reply. The callback leaves `result` alone to pass it on, sets new text with vasdk_result_set
 * (plain text, not JSON), or sets "" to drop it. On a pipeline thread. */
typedef void (*vasdk_text_processor_fn)(const char* text, vasdk_result* result, void* user);
VASDK_C_API int vasdk_session_add_text_processor(vasdk_session* session, vasdk_slot slot, vasdk_text_processor_fn processor,
                                                  void* user);

/* ---- Runtime: what many sessions share, the providers loaded once per conversation running at
 *      the same time (a call center's lines), not once per conversation. ---- */
typedef struct vasdk_runtime vasdk_runtime;
/* An engine config file and the providers it names (both must outlive it). models_dir (may be
 * NULL): used when the config has none. max_sessions: sessions running at once; 0, no limit. */
VASDK_C_API vasdk_runtime* vasdk_runtime_create(const char* config_path, const vasdk_providers* providers, const char* models_dir,
                                                 int max_sessions, char** error);
/* The same, from an engine config the app built (as vasdk_session_create_config). */
VASDK_C_API vasdk_runtime* vasdk_runtime_create_config(const void* config, size_t config_length, vasdk_format config_format,
                                                        const vasdk_providers* providers, const char* models_dir,
                                                        int max_sessions, char** error);
/* After every session made from it is freed. */
VASDK_C_API void vasdk_runtime_free(vasdk_runtime* runtime);
/* A session over the runtime's providers, as vasdk_session_create_message (options: a
 * vasdk.engine.SessionOptions, may be NULL; its models_dir is the runtime's). NULL, with `error`
 * set, at max_sessions. Freeing the session hands its providers back for the next one. */
VASDK_C_API vasdk_session* vasdk_runtime_session_create(vasdk_runtime* runtime, const vasdk_agent* agent, const void* options,
                                                         size_t length, vasdk_format format, char** error);
VASDK_C_API size_t vasdk_runtime_active_sessions(const vasdk_runtime* runtime);
/* Provider sets loaded and waiting for a session. */
VASDK_C_API size_t vasdk_runtime_idle_provider_sets(const vasdk_runtime* runtime);

/* ---- Providers written in the app: added to a vasdk_providers by id, and named by an engine
 *      config like a built-in one (providers { stt { id: "my-stt" } }). Each session gets its own
 *      instance: `create` makes it (its return is the `instance` every other callback gets;
 *      NULL: it failed to start, and the spec's fallback is tried), `destroy` (may be NULL) ends
 *      it. `sink` is how a provider gives back what it finds or makes, from any thread, until
 *      destroy. Audio is 16-bit mono PCM at `sample_rate` (the session's input rate; output rate
 *      for TTS). Callbacks run on pipeline threads. Adding an id again adds another candidate:
 *      the first one added wins. The callbacks and `user` must outlive every session using them. ---- */
typedef struct vasdk_sink vasdk_sink;
typedef void* (*vasdk_provider_create_fn)(vasdk_sink* sink, int sample_rate, void* user);
typedef void (*vasdk_provider_destroy_fn)(void* instance);

/* What a provider gives back. Text: an STT's transcripts (is_final 0 for a partial). Audio: a
 * TTS's speech, in order, then done once the text is all said. Error: what went wrong. */
VASDK_C_API void vasdk_sink_text(vasdk_sink* sink, const char* text, int is_final);
VASDK_C_API void vasdk_sink_audio(vasdk_sink* sink, const short* pcm, size_t samples);
VASDK_C_API void vasdk_sink_done(vasdk_sink* sink);
VASDK_C_API void vasdk_sink_error(vasdk_sink* sink, const char* message);

/* VAD: the chance, 0 to 1, that the audio is speech; 0.5 and above counts as speech. */
typedef float (*vasdk_vad_fn)(void* instance, const short* pcm, size_t samples);
VASDK_C_API int vasdk_providers_add_vad(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                         vasdk_provider_destroy_fn destroy, vasdk_vad_fn detect, void* user);

/* STT. With the config's turn_taking.streaming_transcription off (the default), `write` gets a
 * whole utterance once the user stops, then `end_of_speech`: the final transcript must go to
 * vasdk_sink_text before end_of_speech returns. With it on, `write` gets the audio as they speak,
 * `end_of_speech` (may be NULL) says they stopped, and partials and finals go to vasdk_sink_text
 * whenever ready, from any thread. `start` and `stop` (may be NULL) bracket the session. */
typedef void (*vasdk_stt_event_fn)(void* instance);
typedef void (*vasdk_stt_write_fn)(void* instance, const short* pcm, size_t samples);
VASDK_C_API int vasdk_providers_add_stt(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                         vasdk_provider_destroy_fn destroy, vasdk_stt_event_fn start, vasdk_stt_write_fn write,
                                         vasdk_stt_event_fn end_of_speech, vasdk_stt_event_fn stop, void* user);

/* TTS: `synthesize` starts saying `text` (it may queue it behind the last) and returns nonzero;
 * the speech goes to vasdk_sink_audio, then vasdk_sink_done. `cancel` (may be NULL) drops what's
 * being said and queued, at a barge-in. */
typedef int (*vasdk_tts_synthesize_fn)(void* instance, const char* text);
typedef void (*vasdk_tts_cancel_fn)(void* instance);
VASDK_C_API int vasdk_providers_add_tts(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                         vasdk_provider_destroy_fn destroy, vasdk_tts_synthesize_fn synthesize,
                                         vasdk_tts_cancel_fn cancel, void* user);

/* End of turn: the user's audio since they began speaking; the chance, 0 to 1, that they're done.
 * Asked after they stop (and during pauses when the config's turn-taking asks for it). */
typedef float (*vasdk_turn_fn)(void* instance, const short* pcm, size_t samples);
VASDK_C_API int vasdk_providers_add_turn(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                          vasdk_provider_destroy_fn destroy, vasdk_turn_fn evaluate, void* user);

/* Wake word: every frame of the microphone's audio, in order. When it completes the phrase, the
 * callback sets the phrase heard with vasdk_result_set (plain text) and returns its confidence (0 to 1). */
typedef float (*vasdk_wake_word_fn)(void* instance, const short* pcm, size_t samples, vasdk_result* phrase);
VASDK_C_API int vasdk_providers_add_wake_word(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                               vasdk_provider_destroy_fn destroy, vasdk_wake_word_fn process, void* user);

/* Audio DSP (providers.audio_dsp): every frame of the microphone's audio, in order, before
 * anything hears it; the callback writes as many samples to `output` (which starts as a copy of
 * the input), e.g. with the noise taken out. */
typedef void (*vasdk_audio_dsp_fn)(void* instance, const short* input, short* output, size_t samples);
VASDK_C_API int vasdk_providers_add_audio_dsp(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                               vasdk_provider_destroy_fn destroy, vasdk_audio_dsp_fn process, void* user);

/* Speaker embedding (providers.speaker_embedding): one utterance's audio; the callback sets its
 * voice print with vasdk_result_set_floats, the same length for every utterance (the session
 * normalizes it and matches it to the enrolled speakers'); nothing set: it can't be identified. */
typedef void (*vasdk_speaker_embedding_fn)(void* instance, const short* pcm, size_t samples, vasdk_result* voice_print);
VASDK_C_API int vasdk_providers_add_speaker_embedding(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                                       vasdk_provider_destroy_fn destroy, vasdk_speaker_embedding_fn embed,
                                                       void* user);

/* Sentence encoder (providers.sentence_encoder; one instance, shared by every session of a
 * runtime, called from several threads): the sentence's vector, normalized to length 1, with
 * vasdk_result_set_floats; nothing set: it can't be encoded. */
typedef void (*vasdk_encoder_fn)(void* instance, const char* sentence, vasdk_result* vector);
VASDK_C_API int vasdk_providers_add_sentence_encoder(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                                      vasdk_provider_destroy_fn destroy, vasdk_encoder_fn encode, void* user);

/* Entity model (providers.entity_model; one instance, shared, from several threads): gets
 *   {"words": ["book", "a", "tire", "repair"], "labels": [{"entity", "label", "description", "examples"}], "sensitive": false}
 * and sets with vasdk_result_set a JSON array of where the entities are:
 *   [{"entity": "service", "start": 2, "length": 2, "score": 0.9}]
 * start and length index the words. A sensitive request may hold a secret: don't send it off the device. */
typedef void (*vasdk_entity_model_fn)(void* instance, const char* request_json, vasdk_result* spans);
VASDK_C_API int vasdk_providers_add_entity_model(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                                  vasdk_provider_destroy_fn destroy, vasdk_entity_model_fn extract, void* user);

/* LLM (providers.llm): gets {"messages": [{"role": "system" | "user" | "assistant" | "tool", "content": "..."}]}
 * and sets the reply's text with vasdk_result_set (plain text, not JSON), or fails it with
 * vasdk_result_fail. It may block. The same callback answers the conversation (the system message
 * holds the instructions and the tools, in the session's chat format) and the agent's one-shot asks.
 * `cancel` (may be NULL) gives up the request in flight, from another thread, at a barge-in. */
typedef void (*vasdk_llm_fn)(void* instance, const char* request_json, vasdk_result* reply);
typedef void (*vasdk_llm_cancel_fn)(void* instance);
VASDK_C_API int vasdk_providers_add_llm(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                         vasdk_provider_destroy_fn destroy, vasdk_llm_fn complete, vasdk_llm_cancel_fn cancel,
                                         void* user);

/* ---- Models: the model store (VASDK/ModelStore.h, docs/models.md) downloads the files local
 *      providers load, from a manifest that pins each to a revision and a SHA-256, into the
 *      directory a session reads when its models_dir is unset. ----
 *
 *   vasdk_models* models = vasdk_models_create(NULL, &error);      // VASDK_MODELS_DIR, else the user cache
 *   char* needed = vasdk_models_for_config(models, "engine.txtpb", &error);   // ["silero_vad", ...]
 *   if (!vasdk_models_pull(models, "silero_vad", 0, NULL, NULL, &error)) { ... }
 *   vasdk_models_free(models);
 */
typedef struct vasdk_models vasdk_models;

typedef enum vasdk_model_status {
    VASDK_MODEL_UNKNOWN = -1,   /* no such model in the manifest */
    VASDK_MODEL_MISSING = 0,
    VASDK_MODEL_PARTIAL = 1,    /* some files, or a download that stopped: pull resumes it */
    VASDK_MODEL_PRESENT = 2     /* every file, with its size (vasdk_models_verify checks the hashes) */
} vasdk_model_status;

/* Progress of a pull: bytes of `model` in the store so far, of `total`; `file` is the one
 * downloading ("" once done). Return 0 to stop the pull (it fails with "cancelled" and resumes next time). */
typedef int (*vasdk_model_progress_fn)(const char* model, const char* file, unsigned long long done,
                                       unsigned long long total, void* user);

/* options_json may be NULL: {"directory": "...", "manifest_path": "...", "token": "hf_...",
 * "offline": false, "architecture": "arm64"}. directory unset: VASDK_MODELS_DIR, else the user cache
 * (vasdk_models_default_directory); manifest_path unset: the manifest built into the library; token
 * unset: HF_TOKEN, else `hf auth login`'s. NULL, with `error` set, when the manifest doesn't parse. */
VASDK_C_API vasdk_models* vasdk_models_create(const char* options_json, char** error);
VASDK_C_API void vasdk_models_free(vasdk_models* models);
/* Where sessions look when models_dir is unset, and where the store downloads by default (free it) */
VASDK_C_API char* vasdk_models_default_directory(void);
VASDK_C_API const char* vasdk_models_directory(const vasdk_models* models);
/* Every model of this machine, as JSON: [{"id", "description", "providers": ["vad/silero"], "license",
 * "license_url", "revision", "gated", "requires_acceptance", "size", "status": "present"|"partial"|"missing",
 * "files": [{"path", "url", "sha256", "size"}]}] */
VASDK_C_API char* vasdk_models_list(const vasdk_models* models);
/* The ids of the models an engine config's providers load, as a JSON array. NULL, with `error` set,
 * when the config doesn't load. */
VASDK_C_API char* vasdk_models_for_config(const vasdk_models* models, const char* config_path, char** error);
VASDK_C_API vasdk_model_status vasdk_models_status(const vasdk_models* models, const char* id);
/* Downloads what is missing of the model, checking each file's SHA-256. accept_license: the caller
 * read the license of a model marked requires_acceptance (non-commercial or custom terms). 1 when
 * every file is present; 0, with `error` set, otherwise. Blocks until done: call it off the UI thread. */
VASDK_C_API int vasdk_models_pull(const vasdk_models* models, const char* id, int accept_license,
                                  vasdk_model_progress_fn progress, void* user, char** error);
/* Hashes every file of the model: 1 when all are present and match */
VASDK_C_API int vasdk_models_verify(const vasdk_models* models, const char* id, char** error);
/* Deletes the model's files */
VASDK_C_API int vasdk_models_remove(const vasdk_models* models, const char* id, char** error);

/* ---- Transcript scores (VASDK/metrics/TranscriptScore.h, docs/evaluation.md): how far what a
 *      recognizer heard is from what was said, as word and character error rates. ----
 *
 *   char* score = vasdk_score_transcript("The cat sat.", "the cat sit");
 *   // {"words": {"substitutions": 1, "deletions": 0, "insertions": 0, "reference_length": 3,
 *   //            "error_rate": 0.333}, "characters": {...}, "reference": "the cat sat", "hypothesis": "the cat sit"}
 *   vasdk_free(score);
 *
 * Both texts (UTF-8) are normalized first: lower case, punctuation as spaces, apostrophes kept
 * inside words. Over many clips add the counts up and divide once; the mean of each clip's rate
 * weighs short clips too much. NULL inputs count as empty. Free the result with vasdk_free. */
VASDK_C_API char* vasdk_score_transcript(const char* reference, const char* hypothesis);
/* The text as it is scored (free it) */
VASDK_C_API char* vasdk_normalize_transcript(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* VASDK_C_VASDK_H */
