// Form edge cases: "I don't know" skips a skippable slot and is asked again on a required one; a
// list slot keeps every value named, said back as a list; changing an answer clears the answers
// that depended on it, so they're asked again only if they still apply.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <cstdio>
#include <string>

using namespace va;
using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

const char* kAgent = R"agent(
start_flow: "main"
entity_types { name: "extra" list { values { value: "wash" spoken: "a car wash" synonyms: ["wash", "car wash"] }
                                    values { value: "wax" spoken: "a wax" synonyms: ["wax"] } } }
goals {
  name: "visit"
  require_confirmation: true
  confirm: "So $form.service, $form.extras, notes: $form.notes. Right?"
  slots { name: "service" type: ENUM values: ["oil change", "tire repair"] prompt: "Which service?" }
  slots { name: "size" condition: "$form.service = 'tire repair'" prompt: "What tire size?" }
  slots { name: "extras" entity: "@extra" list: true prompt: "Any extras?" }
  slots { name: "name" prompt: "Your name?" }
  slots { name: "notes" skippable: true prompt: "Anything we should know?" }
}
flows { name: "main" start_page: "visit"
  pages { name: "visit" form { goal: "visit" } routes { condition: "$form.confirmed" target: "done" } }
  pages { name: "done" on_enter { say { text: "Booked." } } on_enter { end {} } } }
)agent";

} // namespace

int main() {
    vasdk::flow::Agent definition;
    if (!google::protobuf::TextFormat::ParseFromString(kAgent, &definition)) check(false, "parse");
    std::string error;
    auto agent = CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;

    FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.text == "Which service?", "asks the service");
    reply = conversation.handleText("a tire repair");
    check(reply.text == "What tire size?", "its condition holds: the size is asked");
    reply = conversation.handleText("seventeen inch");
    check(reply.text == "Any extras?", "then the extras");

    // A list: every value named, in the order said.
    reply = conversation.handleText("a wax and a car wash please");
    check(conversation.lookup({"form", "extras"}).list_value().values_size() == 2, "a list slot keeps both");
    check(reply.text == "Your name?", "then the name");

    // "I don't know": a required slot is asked again; a skippable one is left empty.
    reply = conversation.handleText("I don't know");
    check(reply.event == "no-match" && toText(conversation.lookup({"form", "name"})).empty(), "required: asked again, not stored");
    reply = conversation.handleText("Sam");
    check(reply.text == "Anything we should know?", "the skippable slot is asked");
    reply = conversation.handleText("I don't know");
    check(reply.text == "So tire repair, a wax and a car wash, notes: . Right?", "skipped: empty, and the read-back: " + reply.text);

    // Changing the service clears the size that depended on it; it no longer applies.
    reply = conversation.handleText("no, make it an oil change");
    check(toText(conversation.lookup({"form", "size"})).empty(), "the dependent answer is cleared");
    check(reply.text.rfind("So oil change", 0) == 0, "and the read-back again, without asking the size: " + reply.text);
    // Back to a tire repair: the size applies again, so it's asked.
    // A new value alone at the read-back is a no with it: changed, and read back again.
    reply = conversation.handleText("an oil change");
    check(reply.text == "Please say yes or no. So oil change, a wax and a car wash, notes: . Right?",
          "the same value isn't a change: yes or no is asked again: " + reply.text);
    reply = conversation.handleText("a tire repair");
    check(reply.text == "What tire size?", "a new value alone changes it, and the size is asked again: " + reply.text);

    // Load errors
    std::string text = kAgent;
    text.replace(text.find("slots { name: \"name\" prompt"), std::string("slots { name: \"name\" prompt").size(),
                 "slots { name: \"name\" list: true prompt");
    vasdk::flow::Agent broken;
    (void)google::protobuf::TextFormat::ParseFromString(text, &broken);
    check(!CompiledAgent::compile(broken, &error) && error.find("list needs an entity or ENUM values") != std::string::npos,
          "a list of free text");

    if (failures == 0) std::printf("form_edge_cases_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
