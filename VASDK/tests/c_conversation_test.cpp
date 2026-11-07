// What the C API's text conversations and turns do beyond turns in and replies out: a page type's
// turn reads its flow, page and the reply so far, renders a template, asks through a StepOutcome
// and rewrites the user's words; a conversation takes dynamic entity values and the app's
// context, snapshots and resumes, tells its no-input wait and trigger state, checks its triggers;
// a FlowTestCase runs from memory. Only C API calls, the messages built with their protos.

#include <VASDK/c/vasdk.h>

#include <google/protobuf/struct.pb.h>
#include <vasdk/engine/events.pb.h>
#include <vasdk/flow/conversation.pb.h>
#include <vasdk/flow/snapshot.pb.h>
#include <vasdk/flow/step_outcome.pb.h>
#include <vasdk/flow/test_case.pb.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string take(char* text) {
    std::string out = text ? text : "";
    vasdk_free(text);
    return out;
}

// What the page type's callbacks saw.
struct Seen {
    std::string flow, page, rendered, replySoFar;
};

const char* kAgent = R"(
  start_flow: "main"
  session_params { name: "tier" }
  entity_types { name: "contact" dynamic: true list { values { value: "nobody" } } }
  intents { name: "call" examples: ["call @contact"] }
  flows { name: "main" start_page: "probe"
    pages { name: "probe" type: "probe"
            routes { intent: "call" steps { say { text: "Calling $nlu.entities.contact." } } target: "where" } }
    pages { name: "where" on_enter { say { text: "You're in $context.city." } } }
  }
)";

// The page asks, through a StepOutcome, after reading where it is.
void probeEnter(const char*, vasdk_turn* turn, void* user) {
    auto& seen = *static_cast<Seen*>(user);
    seen.flow = take(vasdk_turn_flow(turn));
    seen.page = take(vasdk_turn_page(turn));
    seen.rendered = take(vasdk_turn_render(turn, "Tier $session.tier"));
    vasdk::flow::StepOutcome outcome;
    outcome.add_say("Welcome.");
    outcome.mutable_ask()->set_question("Who should I call?");
    const std::string bytes = outcome.SerializeAsString();
    vasdk_turn_apply(turn, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, nullptr);
    seen.replySoFar = vasdk_turn_reply_text(turn);
}

// "phone someone" is read as "call mom"; the routes answer it.
int probeTurn(const char*, vasdk_turn* turn, void*) {
    if (std::string(vasdk_turn_text(turn)) == "phone someone") vasdk_turn_set_text(turn, "call mom");
    return 0;
}

vasdk::flow::ConversationReply send(vasdk_conversation* conversation, const vasdk::flow::ConversationInput& input) {
    const std::string bytes = input.SerializeAsString();
    size_t length = 0;
    char* error = nullptr;
    void* data = vasdk_conversation_send(conversation, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &length, &error);
    vasdk::flow::ConversationReply reply;
    if (data) reply.ParseFromArray(data, static_cast<int>(length));
    vasdk_free(data);
    vasdk_free(error);
    return reply;
}

} // namespace

int main() {
    vasdk_set_log_level("warn");
    Seen seen;
    vasdk_registry* registry = vasdk_registry_create();
    check(vasdk_registry_add_page_type(registry, "probe", nullptr, probeEnter, probeTurn, &seen), "a page type registers");
    char* error = nullptr;
    vasdk_agent* agent = vasdk_agent_parse(kAgent, std::string(kAgent).size(), VASDK_FORMAT_TEXT, registry, &error);
    check(agent != nullptr, std::string("the agent compiles: ") + (error ? error : ""));
    vasdk_free(error);
    error = nullptr;
    if (!agent) return 1;
    vasdk_conversation* conversation = vasdk_conversation_create(agent);

    check(vasdk_conversation_set_param(conversation, "tier", "\"gold\"", &error), "a session param");
    vasdk::engine::EntityValuesCommand contacts;
    contacts.set_name("contact");
    auto* mom = contacts.add_values();
    mom->set_value("mom");
    mom->set_spoken("Mom");
    mom->add_synonyms("mom");
    std::string bytes = contacts.SerializeAsString();
    check(vasdk_conversation_set_entity_values_message(conversation, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &error),
          "a dynamic entity's values");
    vasdk::engine::EntityValuesCommand notDynamic;
    notDynamic.set_name("nothing");
    bytes = notDynamic.SerializeAsString();
    check(!vasdk_conversation_set_entity_values_message(conversation, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &error) &&
              error,
          "an entity that isn't dynamic is refused");
    vasdk_free(error);
    error = nullptr;
    google::protobuf::Struct context;
    (*context.mutable_fields())["city"].set_string_value("Paris");
    bytes = context.SerializeAsString();
    check(vasdk_conversation_set_context_message(conversation, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &error), "the app's context");

    vasdk::flow::ConversationInput input;
    input.set_start(true);
    auto reply = send(conversation, input);
    check(contains(reply.text(), "Welcome.") && contains(reply.text(), "Who should I call?") && reply.expects_reply(),
          "a page type's StepOutcome says, then asks");
    check(seen.flow == "main" && seen.page == "probe", "a turn knows its flow and page");
    check(seen.rendered == "Tier gold", "a turn renders a template");
    check(contains(seen.replySoFar, "Who should I call?"), "a turn reads the reply so far");

    input.Clear();
    input.set_text("phone someone");
    reply = send(conversation, input);
    check(contains(reply.text(), "Calling Mom.") && contains(reply.text(), "You're in Paris.") && reply.page() == "where",
          "set_text rewrote the words for the routes; the dynamic value and the context were read (got: " + reply.text() + ")");

    size_t length = 0;
    void* snapshot = vasdk_conversation_snapshot_message(conversation, VASDK_FORMAT_BINARY, &length);
    vasdk::flow::ConversationSnapshot parsed;
    check(snapshot && parsed.ParseFromArray(snapshot, static_cast<int>(length)) && parsed.page() == "where", "a snapshot of where it is");
    vasdk_conversation* again = vasdk_conversation_create(agent);
    int resumed = 0;
    size_t replyLength = 0;
    void* resumedReply = vasdk_conversation_resume_message(again, snapshot, length, VASDK_FORMAT_BINARY, &replyLength, &resumed, &error);
    vasdk::flow::ConversationReply afterResume;
    check(resumedReply && resumed && afterResume.ParseFromArray(resumedReply, static_cast<int>(replyLength)) &&
              afterResume.page() == "where",
          "another conversation resumes there");
    vasdk_free(resumedReply);
    vasdk_free(snapshot);
    vasdk_conversation_free(again);

    vasdk_conversation_reply_interrupted(conversation);
    check(vasdk_conversation_no_input_ms(conversation) >= 0, "the no-input wait");
    check(vasdk_conversation_next_scheduled_trigger_ms(conversation) == -1, "no schedule trigger");
    const std::string state = take(vasdk_conversation_trigger_state(conversation));
    check(vasdk_conversation_restore_trigger_state(conversation, state.c_str()), "the trigger state restores");
    input.Clear();
    input.set_check_triggers(true);
    reply = send(conversation, input);
    check(reply.text().empty(), "check_triggers with none due says nothing");
    input.Clear();
    input.set_deliver_queued(true);
    reply = send(conversation, input);
    check(reply.text().empty(), "deliver_queued with none queued says nothing");

    vasdk::flow::FlowTestCase test;
    test.add_turns()->mutable_expect()->add_said_contains("Welcome.");
    bytes = test.SerializeAsString();
    char* report = nullptr;
    check(vasdk_run_test_message(agent, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &report), "a FlowTestCase from memory passes");
    vasdk_free(report);
    report = nullptr;
    test.mutable_turns(0)->mutable_expect()->add_said_contains("Goodbye.");
    bytes = test.SerializeAsString();
    check(!vasdk_run_test_message(agent, bytes.data(), bytes.size(), VASDK_FORMAT_BINARY, &report) && report && *report,
          "one that fails says what differed");
    vasdk_free(report);

    vasdk_conversation_free(conversation);
    vasdk_agent_free(agent);
    vasdk_registry_free(registry);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("C conversations ok");
    return 0;
}
