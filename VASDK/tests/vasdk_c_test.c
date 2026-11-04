/* The C API from C: an agent with a step, a tool and an envelope written in C; a conversation
 * by text; session params; a StepOutcome refused for a key it may not write; a test case file. */

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

static const char* kAgent =
    "start_flow: \"main\"\n"
    "session_params { name: \"tier\" type: STRING default { string_value: \"standard\" } }\n"
    "session_params { name: \"points\" type: NUMBER }\n"
    "session_params { name: \"booking\" type: STRUCT }\n"
    "envelopes: [\"thanks\"]\n"
    "intents { name: \"points\" examples: [\"points\", \"my points\"] }\n"
    "intents { name: \"book\" examples: [\"book\"] }\n"
    "flows { name: \"main\" start_page: \"welcome\" pages {\n"
    "  name: \"welcome\"\n"
    "  on_enter { ask { say { text: \"Hi, how can I help?\" } } }\n"
    "  routes { intent: \"points\" steps { custom { [type.googleapis.com/google.protobuf.Struct] {\n"
    "    fields { key: \"into\" value { string_value: \"$session.points\" } } } } } }\n"
    "  routes { intent: \"book\" steps { call_tool { name: \"create_booking\" args { key: \"service\" value: \"'oil'\" }\n"
    "    into: \"$session.booking\" } } steps { say { text: \"Booked $session.booking.reference.\" } } } } }\n";

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
