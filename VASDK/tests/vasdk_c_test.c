/* The C API from C: an agent with a step, a tool, an envelope, a page type and an entity
 * extractor written in C; a conversation by text; session params; a StepOutcome refused for a key
 * it may not write; a test case file. */

#include <VASDK/c/vasdk.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(int ok, const char* what) {
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static int refused = 0;

/* custom { [type.googleapis.com/google.protobuf.Struct] { ... } }: loyalty points by tier. */
static void points_step(const char* params_json, vasdk_turn* turn, void* user) {
    (void)user;
    check(strstr(params_json, "\"into\"") != NULL, "the step gets its params as JSON");
    char* tier = vasdk_turn_lookup(turn, "$session.tier");
    const int gold = strcmp(tier, "\"gold\"") == 0;
    vasdk_free(tier);
    const char* outcome = gold ? "{\"set\": {\"$session.points\": 120}, \"say\": [\"You have 120 points.\"]}"
                               : "{\"say\": [\"Members earn points.\"]}";
    char* error = NULL;
    check(vasdk_turn_apply(turn, outcome, strlen(outcome), VASDK_FORMAT_JSON, &error) == 1, "a StepOutcome applies");
    vasdk_free(error);
    const char* forbidden = "{\"set\": {\"$input.text\": \"x\"}}";
    if (vasdk_turn_apply(turn, forbidden, strlen(forbidden), VASDK_FORMAT_JSON, &error) == 0 && error) refused = 1;
    vasdk_free(error);
}

static void create_booking(const char* args_json, vasdk_result* result, void* user) {
    int* calls = (int*)user;
    ++*calls;
    if (strstr(args_json, "\"oil\"") == NULL) {
        vasdk_result_fail(result, "unknown service");
        return;
    }
    vasdk_result_set(result, "{\"reference\": \"B-42\"}");
}

static int thanks_before(vasdk_turn* turn, void* user) {
    (void)user;
    if (strcmp(vasdk_turn_text(turn), "thanks") != 0) return 0;
    vasdk_turn_say(turn, "You're welcome.");
    return 1;
}

/* page type "quiz": params {"question", "answer"}; asks on entering, and answers a right guess itself. */
static void quiz_enter(const char* params_json, vasdk_turn* turn, void* user) {
    (void)user;
    check(strstr(params_json, "\"question\"") != NULL, "the page type gets its params as JSON");
    vasdk_turn_say(turn, "What colour is the sky?");
}

static int quiz_turn(const char* params_json, vasdk_turn* turn, void* user) {
    (void)user;
    if (strstr(params_json, "\"answer\":\"blue\"") == NULL || strcmp(vasdk_turn_text(turn), "blue") != 0) return 0;
    vasdk_turn_say(turn, "Right!");
    vasdk_turn_goto(turn, "welcome");
    return 1;
}

/* code { extractor: "contacts" }: "mom" is a contact, anywhere in the words. */
static void contacts(const char* request_json, vasdk_result* result, void* user) {
    (void)user;
    const char* words = strstr(request_json, "\"words\":[");
    check(words != NULL, "the extractor gets the words as JSON");
    const char* found = words ? strstr(words, "\"mom\"") : NULL;
    if (!found) {
        vasdk_result_set(result, "[]");
        return;
    }
    int index = 0;   /* the commas before it in the array: its place among the words */
    for (const char* c = words; c < found; ++c)
        if (*c == ',') ++index;
    char json[160];
    snprintf(json, sizeof json,
             "[{\"start\": %d, \"value\": \"+15550100\", \"spoken\": \"Mom\", \"attributes\": {\"label\": \"mobile\"}}]", index);
    vasdk_result_set(result, json);
}

static const char* kAgent =
    "start_flow: \"main\"\n"
    "session_params { name: \"tier\" type: STRING default { string_value: \"standard\" } }\n"
    "session_params { name: \"points\" type: NUMBER }\n"
    "session_params { name: \"booking\" type: STRUCT }\n"
    "envelopes: [\"thanks\"]\n"
    "intents { name: \"points\" examples: [\"points\", \"my points\"] }\n"
    "intents { name: \"book\" examples: [\"book\"] }\n"
    "intents { name: \"quiz\" examples: [\"quiz\", \"a quiz\"] }\n"
    "intents { name: \"call\" examples: [\"call\", \"make a call\"] }\n"
    "entity_types { name: \"contact\" code { extractor: \"contacts\" } }\n"
    "goals { name: \"calling\" slots { name: \"who\" entity: \"@contact\" prompt: \"Who should I call?\" } }\n"
    "flows { name: \"main\" start_page: \"welcome\" pages {\n"
    "  name: \"welcome\"\n"
    "  on_enter { ask { say { text: \"Hi, how can I help?\" } } }\n"
    "  routes { intent: \"points\" steps { custom { [type.googleapis.com/google.protobuf.Struct] {\n"
    "    fields { key: \"into\" value { string_value: \"$session.points\" } } } } } }\n"
    "  routes { intent: \"book\" steps { call_tool { name: \"create_booking\" args { key: \"service\" value: \"'oil'\" }\n"
    "    into: \"$session.booking\" } } steps { say { text: \"Booked $session.booking.reference.\" } } }\n"
    "  routes { intent: \"quiz\" target: \"quiz\" }\n"
    "  routes { intent: \"call\" target: \"calling\" } }\n"
    "  pages { name: \"quiz\" type: \"quiz\" params { [type.googleapis.com/google.protobuf.Struct] {\n"
    "    fields { key: \"question\" value { string_value: \"sky\" } } fields { key: \"answer\" value { string_value: \"blue\" } } } }\n"
    "    routes { intent: \"points\" steps { say { text: \"No points in a quiz.\" } } } }\n"
    "  pages { name: \"calling\" form { goal: \"calling\" }\n"
    "    routes { condition: \"$form.complete\" steps { say { text: \"Calling $form.who ($form.who.label).\" } } target: \"welcome\" } } }\n";

static const char* kCase =
    "name: \"points by tier\"\n"
    "session { fields { key: \"tier\" value { string_value: \"gold\" } } }\n"
    "turns { expect { page: \"welcome\" } }\n"
    "turns { user: \"my points\" expect { said_contains: \"120 points\" } }\n";

int main(void) {
    int bookings = 0;
    vasdk_registry* registry = vasdk_registry_create();
    check(vasdk_registry_add_step(registry, "google.protobuf.Struct", points_step, NULL), "a step registers");
    check(vasdk_registry_add_tool(registry, "create_booking", "Books a service", create_booking, &bookings), "a tool registers");
    check(vasdk_registry_add_envelope(registry, "thanks", thanks_before, NULL, NULL), "an envelope registers");
    check(vasdk_registry_add_page_type(registry, "quiz", "google.protobuf.Struct", quiz_enter, quiz_turn, NULL),
          "a page type registers");
    check(vasdk_registry_add_entity_extractor(registry, "contacts", contacts, NULL), "an entity extractor registers");

    char* error = NULL;
    vasdk_agent* broken = vasdk_agent_parse("flows {", 7, VASDK_FORMAT_TEXT, registry, &error);
    check(broken == NULL && error != NULL, "a broken agent fails, with the reason");
    vasdk_free(error);
    error = NULL;
    vasdk_agent* agent = vasdk_agent_parse(kAgent, strlen(kAgent), VASDK_FORMAT_TEXT, registry, &error);
    if (!agent) {
        fprintf(stderr, "agent: %s\n", error ? error : "?");
        return 1;
    }

    vasdk_conversation* conversation = vasdk_conversation_create(agent);
    check(vasdk_conversation_set_param(conversation, "tier", "\"gold\"", NULL) == 1, "a session param from JSON");
    check(vasdk_conversation_set_param(conversation, "nope", "1", &error) == 0 && error, "an undeclared param fails");
    vasdk_free(error);

    char* reply = vasdk_conversation_start(conversation);
    check(strstr(reply, "\"text\":\"Hi, how can I help?\"") != NULL && strstr(reply, "\"page\":\"welcome\"") != NULL,
          "the greeting, as JSON");
    vasdk_free(reply);

    reply = vasdk_conversation_say(conversation, "my points");
    check(strstr(reply, "You have 120 points.") != NULL, "the C step said its outcome");
    vasdk_free(reply);
    char* points = vasdk_conversation_lookup(conversation, "$session.points");
    check(strcmp(points, "120") == 0, "and set the session param");
    vasdk_free(points);
    check(refused, "a key it may not write is refused");

    reply = vasdk_conversation_say(conversation, "thanks");
    check(strstr(reply, "You're welcome.") != NULL, "the C envelope answered");
    vasdk_free(reply);

    reply = vasdk_conversation_say(conversation, "book");
    check(strstr(reply, "Booked B-42.") != NULL && strstr(reply, "\"name\":\"create_booking\"") != NULL && bookings == 1,
          "the C tool was called and its result stored");
    vasdk_free(reply);

    reply = vasdk_conversation_say(conversation, "a quiz");
    check(strstr(reply, "What colour is the sky?") != NULL && strstr(reply, "\"page\":\"quiz\"") != NULL,
          "the C page type asked on entering");
    vasdk_free(reply);
    reply = vasdk_conversation_say(conversation, "my points");
    check(strstr(reply, "No points in a quiz.") != NULL, "a turn it doesn't answer goes to the page's routes");
    vasdk_free(reply);
    reply = vasdk_conversation_say(conversation, "blue");
    check(strstr(reply, "Right!") != NULL && strstr(reply, "\"page\":\"welcome\"") != NULL,
          "the C page type answered the turn and moved on");
    vasdk_free(reply);

    reply = vasdk_conversation_say(conversation, "make a call");
    check(strstr(reply, "Who should I call?") != NULL, "the form asks for the contact");
    vasdk_free(reply);
    reply = vasdk_conversation_say(conversation, "my mom please");
    check(strstr(reply, "Calling Mom (mobile).") != NULL, "the C extractor found the contact, with its spoken form and attribute");
    if (strstr(reply, "Calling Mom (mobile).") == NULL) fprintf(stderr, "  reply: %s\n", reply);
    vasdk_free(reply);

    /* A test case file, played through the same agent. */
    FILE* file = fopen("vasdk_c_test_case.txtpb", "w");
    if (file) {
        fputs(kCase, file);
        fclose(file);
    }
    char* report = NULL;
    check(vasdk_run_test_file(agent, "vasdk_c_test_case.txtpb", &report) == 1, "a test case file passes");
    if (report && *report) fprintf(stderr, "%s", report);
    vasdk_free(report);
    remove("vasdk_c_test_case.txtpb");

    check(strlen(vasdk_version()) > 0, "a version");
    vasdk_conversation_free(conversation);
    vasdk_agent_free(agent);
    vasdk_registry_free(registry);
    if (failures == 0) printf("vasdk_c_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
