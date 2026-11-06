// Forms: values read from what the user says (list values, numbers in digits or words, yes/no,
// spoken phone digits), a booking completed by text, a slot asked only when its condition holds,
// reprompts and failing after max_reprompts, "no" to the read-back with and without a new value,
// clear_slot, intent routes while a slot is asked, set $form.<slot>, undo by copy, and lint errors.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/flow/SlotExtraction.h>

#include <google/protobuf/text_format.h>

#include <cstdio>
#include <string>

using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kAgent = R"agent(
start_flow: "main"
intents { name: "book" examples: ["book"] }
intents { name: "person" examples: ["a person"] }
intents { name: "change_day" examples: ["change the day"] }
session_params { name: "service" type: STRING }
session_params { name: "day" type: STRING }
session_params { name: "phone" type: STRING }
goals {
  name: "booking"
  require_confirmation: true
  confirm: "So that's $form.service on $form.day, and we'll call $form.phone. Shall I book it?"
  slots { name: "service" type: ENUM values: ["oil change", "tire repair", "tire"] prompt: "Which service?" }
  slots { name: "tire_count" type: INTEGER min: 1 max: 4 condition: "$form.service = 'tire repair'"
          prompt: "How many tires?" }
  slots { name: "day" type: ENUM values: ["monday", "tuesday", "saturday"] prompt: "Which day works for you?" }
  slots { name: "phone" type: PHONE prompt: "What's the best number to reach you?" }
  slots { name: "notes" type: STRING optional: true }
}
flows {
  name: "main"
  start_page: "welcome"
  pages { name: "welcome" on_enter { ask { say { text: "Hi. Want to book?" } } }
          routes { intent: "book" target: "booking" } }
  pages {
    name: "booking"
    on_enter { branch { if: "exists($session.phone)" then { set { key: "$form.phone" value: "$session.phone" } } } }
    form { goal: "booking" }
    routes { condition: "$form.confirmed"
             steps { set { key: "$session.service" value: "$form.service" } }
             steps { set { key: "$session.day" value: "$form.day" } }
             target: "done" }
    routes { condition: "$form.failed" target: "handoff" }
    routes { intent: "person" target: "handoff" }
    routes { intent: "change_day" steps { clear_slot { slot: "day" } } }
  }
  pages { name: "done" on_enter { say { text: "You're booked for $session.service on $session.day." } } on_enter { end {} } }
  pages { name: "handoff" on_enter { say { text: "Connecting you to a person." } } on_enter { end {} } }
}
)agent";

std::shared_ptr<const CompiledAgent> compileText(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error);
}

std::string lintError(const std::string& replace, const std::string& with) {
    std::string text = kAgent;
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    text.replace(at, replace.size(), with);
    std::string error;
    return compileText(text, &error) ? "" : error;
}

vasdk::goal::Slot slotOf(const char* text) {
    vasdk::goal::Slot slot;
    google::protobuf::TextFormat::ParseFromString(text, &slot);
    return slot;
}

std::string form(const FlowRuntime& conversation, const std::string& name) {
    return toText(conversation.lookup({"form", name}));
}

} // namespace

int main() {
    // Reading values from what was said, in VASDK's built-in en-US
    const EntityCatalog english(std::make_shared<const va::nlu::Languages>(va::nlu::Languages({va::nlu::Language::builtIn()})));
    const auto service = slotOf(R"(type: ENUM values: ["oil change", "tire", "tire repair"])");
    check(extractSlotValue(service, "I'd like an Oil-Change please", english) == std::optional<std::string>("oil change"),
          "a list value, any case and punctuation");
    check(extractSlotValue(service, "a tire repair", english) == std::optional<std::string>("tire repair"), "the longest value wins");
    check(!extractSlotValue(service, "a car wash", english), "no list value");
    const auto count = slotOf("type: INTEGER");
    check(extractSlotValue(count, "twenty five", english) == std::optional<std::string>("25") &&
              extractSlotValue(count, "I have 3 cars", english) == std::optional<std::string>("3") &&
              extractSlotValue(count, "a hundred and two", english) == std::optional<std::string>("102") &&
              extractSlotValue(count, "twenty-one, please", english) == std::optional<std::string>("21"),
          "integers in digits and words");
    check(!extractSlotValue(count, "lots", english), "no number");
    check(extractSlotValue(slotOf("type: NUMBER"), "about 2.5 liters", english) == std::optional<std::string>("2.5"), "a decimal");
    const auto yesNo = slotOf("type: BOOLEAN");
    check(extractSlotValue(yesNo, "yeah sure", english) == std::optional<std::string>("yes") &&
              extractSlotValue(yesNo, "nope", english) == std::optional<std::string>("no"), "yes and no");
    check(extractYesNo("no, yes", english) == std::optional<bool>(false) && !extractYesNo("maybe later", english), "the first answer wins");
    check(extractSlotValue(slotOf("type: PHONE"), "five five five, one two three, double four six seven", english) ==
              std::optional<std::string>("5551234467"), "spoken phone digits");

    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the form agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    // A booking completed by text; the service named on the way in is already filled.
    FlowRuntime booking(agent);
    booking.start();
    auto reply = booking.handleText("I want to book an oil change");
    check(booking.pageName() == "booking" && form(booking, "service") == "oil change" &&
              reply.text == "Which day works for you?" && reply.expectsReply,
          "the service from the way in, then the day is asked");
    reply = booking.handleText("Tuesday please");
    check(form(booking, "day") == "tuesday" && reply.text == "What's the best number to reach you?", "day, then phone");
    reply = booking.handleText("five five five one two three four four six seven");
    check(reply.text == "So that's oil change on tuesday, and we'll call 555-123-4467. Shall I book it?" &&
              form(booking, "complete") == "true" && form(booking, "confirmed") == "false",
          "the values read back");
    reply = booking.handleText("yes");
    check(booking.pageName() == "done" && reply.text == "You're booked for oil change on tuesday." && reply.endsSession,
          "confirmed: booked");

    // A slot asked only when its condition holds.
    FlowRuntime tires(agent);
    tires.start();
    reply = tires.handleText("book a tire repair");
    check(reply.text == "How many tires?", "the condition slot is asked for a tire repair");
    reply = tires.handleText("three");
    check(form(tires, "tire_count") == "3" && reply.text == "Which day works for you?", "number words fill it");
    reply = tires.handleText("seven");
    check(!reply.matched && form(tires, "day").empty() && reply.text == "Sorry, I didn't get that. Which day works for you?",
          "an answer that doesn't fit is asked again");

    // Answers that never fit: reprompts, then $form.failed takes its route.
    FlowRuntime failing(agent);
    failing.start();
    failing.handleText("book an oil change on monday");
    check(form(failing, "day") == "monday", "two values from one answer");
    reply = failing.handleText("one two three");
    check(!reply.matched && contains(reply.text, "I only got 3 digits.") && form(failing, "phone").empty(),
          "three digits aren't a phone number: too few, said so");
    failing.handleText("blue");
    reply = failing.handleText("green");
    check(failing.pageName() == "handoff", "max_reprompts 2: the third miss fails");

    // "No" to the read-back: with a new value it changes at once; alone it asks what to change.
    FlowRuntime changing(agent);
    changing.start();
    changing.handleText("book an oil change");
    changing.handleText("monday");
    changing.handleText("555 123 4467");
    reply = changing.handleText("no, make it saturday");
    check(form(changing, "day") == "saturday" && contains(reply.text, "on saturday") && contains(reply.text, "Shall I book it?"),
          "a new value in the no: read back again");
    reply = changing.handleText("no");
    check(reply.text == "What would you like to change?", "a plain no asks what to change");
    reply = changing.handleText("the phone");
    check(form(changing, "phone").empty() && reply.text == "What's the best number to reach you?", "the named slot is asked again");
    reply = changing.handleText("555 987 6543");
    check(contains(reply.text, "555-987-6543") && contains(reply.text, "Shall I book it?"), "and confirmed again");

    // A route while the form asks: clear_slot, and an intent that leaves.
    reply = changing.handleText("actually change the day");
    check(form(changing, "day").empty() && reply.text == "Which day works for you?", "clear_slot: the day is asked again");
    changing.handleText("I'd rather talk to a person");
    check(changing.pageName() == "handoff", "an intent route still works during the form");

    // set $form.<slot> from session data: the phone isn't asked.
    FlowRuntime known(agent);
    known.setSessionParam("phone", stringValue("5551112222"));
    known.start();
    known.handleText("book a tire repair");
    known.handleText("two");
    reply = known.handleText("monday");
    check(contains(reply.text, "we'll call 555-111-2222"), "prefilled from the session");

    // Undo: a copy keeps its own form values.
    FlowRuntime original(agent);
    original.start();
    original.handleText("book an oil change");
    const FlowRuntime before = original;
    original.handleText("monday");
    check(form(original, "day") == "monday" && form(before, "day").empty() && form(before, "service") == "oil change",
          "a copy is independent");

    // Lint
    check(contains(lintError("form { goal: \"booking\" }", "form { goal: \"bookings\" }"), "unknown goal \"bookings\""),
          "unknown goal");
    check(contains(lintError("say { text: \"You're booked", "say { text: \"$form.day You're booked"), "the page has no form"),
          "$form on a page without a form");
    check(contains(lintError("values: [\"monday\", \"tuesday\", \"saturday\"] ", ""), "is an ENUM with no values"),
          "an ENUM needs values");
    check(contains(lintError("condition: \"$form.service = 'tire repair'\"", "condition: \"$session.service = 'x'\""),
                   "a slot condition reads only $form.<slot>"),
          "slot conditions read the form only");
    check(contains(lintError("clear_slot { slot: \"day\" }", "clear_slot { slot: \"date\" }"), "has no slot \"date\""),
          "clear_slot of an unknown slot");
    check(contains(lintError("  confirm: \"So that's", "  objective: \"So that's"), "no confirm text"),
          "require_confirmation needs a confirm text");

    if (failures == 0) std::printf("form_primitives_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
