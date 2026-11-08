// Triggers: what starts or interrupts a conversation, and how politely. A session-start trigger
// greets a returning caller; an app event waits for the booking to finish (AFTER_FLOW), for the
// reply to end (AFTER_TURN), or comes at once (IMMEDIATE); NEVER only outside a conversation.
// Queued ones expire, duplicates and cooled-down ones are dropped, quiet hours suppress all but
// CRITICAL, an hourly limit caps them, and ask_first asks before running the action.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

using namespace va;
using namespace va::flow;
using Decision = vasdk::flow::TriggerDecision;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

std::string hhmm(int minuteOfDay) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", (minuteOfDay / 60) % 24, minuteOfDay % 60);
    return text;
}

int localMinuteOfDay(std::chrono::system_clock::time_point at) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local.tm_hour * 60 + local.tm_min;
}

// Quiet hours from two hours after `start` to three hours after it: `start` itself isn't quiet.
std::string agentText(std::chrono::system_clock::time_point start) {
    const int minute = localMinuteOfDay(start);
    return R"agent(
start_flow: "main"
session_params { name: "name" type: STRING default { string_value: "Sam" } }
intents { name: "book" examples: ["book"] }
intents { name: "hours" examples: ["hours"] }
goals { name: "booking" slots { name: "day" prompt: "Which day?" } }
proactive_policy { quiet_from: ")agent" + hhmm(minute + 120) + R"agent(" quiet_until: ")agent" + hhmm(minute + 180) +
           R"agent(" max_per_hour: 6 }
triggers { name: "welcome" source { session_start {} } condition: "$session.name = 'Sam'"
           action { say { text: "Welcome back, $session.name." } } }
triggers { name: "car_ready" source { app_event { name: "car-ready" } } action { say { text: "Your $trigger.payload.car is ready." } }
           delivery { interrupt: AFTER_FLOW priority: HIGH dedupe_key: "$trigger.payload.car" } }
triggers { name: "promo" source { app_event {} } action { say { text: "We have a promo." } }
           delivery { interrupt: AFTER_TURN expires_after_ms: 1000 } }
triggers { name: "alarm" source { app_event {} } action { say { text: "Alarm!" } } delivery { interrupt: IMMEDIATE priority: CRITICAL } }
triggers { name: "battery" source { app_event {} } action { say { text: "Battery low." } } delivery { interrupt: NEVER cooldown_ms: 60000 } }
triggers { name: "survey" source { app_event {} } action { start_flow: "survey" }
           delivery { ask_first { text: "Got a minute for a survey?" } } }
triggers { name: "nudge" source { inactivity { after_ms: 30000 } } action { say { text: "Still there?" } } }
flows {
  name: "main"
  start_page: "welcome"
  pages { name: "welcome" on_enter { ask { say { text: "Book or hours?" } } }
          routes { intent: "book" target: "booking" }
          routes { intent: "hours" steps { say { text: "8 to 6." } } } }
  pages { name: "booking" form { goal: "booking" }
          routes { condition: "$form.complete" steps { say { text: "Booked." } } target: "welcome" } }
}
flows { name: "survey" start_page: "question"
        pages { name: "question" on_enter { say { text: "Rate us." } } on_enter { goto { target: "END_FLOW" } } } }
)agent";
}

std::shared_ptr<const CompiledAgent> compile(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error);
}

Decision decisionOf(const Reply& reply, const std::string& name) {
    for (const auto& trigger : reply.triggers)
        if (trigger.name == name) return trigger.decision;
    return vasdk::flow::DECISION_UNSET;
}

google::protobuf::Struct car(const std::string& name) {
    google::protobuf::Struct payload;
    (*payload.mutable_fields())["car"] = stringValue(name);
    return payload;
}

} // namespace

int main() {
    // 2026-11-05, noon, local time: the quiet hours are later.
    std::tm noon{};
    noon.tm_year = 2026 - 1900;
    noon.tm_mon = 10;
    noon.tm_mday = 5;
    noon.tm_hour = 12;
    noon.tm_isdst = -1;
    auto now = std::chrono::system_clock::from_time_t(std::mktime(&noon));
    std::string error;
    auto agent = compile(agentText(now), &error);
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    auto conversationAt = [&](std::chrono::system_clock::time_point& clock) {
        FlowRuntime conversation(agent);
        conversation.setClock([&clock] { return clock; });
        return conversation;
    };

    // A session-start trigger: said first, then the start page.
    auto conversation = conversationAt(now);
    auto reply = conversation.start();
    check(reply.text == "Welcome back, Sam. Book or hours?", "session start: the trigger, then the greeting");

    // AFTER_FLOW: waits while the booking form is being filled; a duplicate is dropped.
    conversation.handleText("book");
    reply = conversation.fireTrigger("car-ready", car("Golf"));
    check(decisionOf(reply, "car_ready") == vasdk::flow::QUEUED && reply.text.empty(), "AFTER_FLOW during a form: queued");
    reply = conversation.fireTrigger("car-ready", car("Golf"));
    check(decisionOf(reply, "car_ready") == vasdk::flow::DROPPED, "the same dedupe key: dropped");
    reply = conversation.handleText("tuesday");
    check(reply.text == "Booked. Book or hours? Your Golf is ready." && decisionOf(reply, "car_ready") == vasdk::flow::STARTED,
          "delivered once the booking is done");
    if (reply.text != "Booked. Book or hours? Your Golf is ready.") std::fprintf(stderr, "  %s\n", reply.text.c_str());

    // AFTER_TURN: at once between turns, the question again after it; queued during a reply.
    reply = conversation.fireTrigger("promo");
    check(reply.text == "We have a promo. Book or hours?" && decisionOf(reply, "promo") == vasdk::flow::STARTED,
          "AFTER_TURN between turns: now, then the question");
    reply = conversation.fireTrigger("promo", {}, false);
    check(decisionOf(reply, "promo") == vasdk::flow::QUEUED, "AFTER_TURN during a reply: queued");
    reply = conversation.deliverQueued();
    check(reply.text == "We have a promo. Book or hours?", "delivered once the reply has played");

    // Expiry: queued past expires_after_ms.
    conversation.fireTrigger("promo", {}, false);
    now += std::chrono::seconds(2);
    reply = conversation.deliverQueued();
    check(reply.text.empty() && decisionOf(reply, "promo") == vasdk::flow::EXPIRED, "expired");

    // IMMEDIATE: even mid-form.
    conversation.handleText("book");
    reply = conversation.fireTrigger("alarm", {}, false);
    check(reply.text == "Alarm! Which day?" && decisionOf(reply, "alarm") == vasdk::flow::STARTED, "IMMEDIATE: now");

    // NEVER: not during a conversation; outside one it comes, then cools down.
    reply = conversation.fireTrigger("battery");
    check(decisionOf(reply, "battery") == vasdk::flow::DROPPED, "NEVER mid-conversation: dropped");
    auto idle = conversationAt(now);
    reply = idle.fireTrigger("battery");
    check(reply.text == "Battery low." && decisionOf(reply, "battery") == vasdk::flow::STARTED, "NEVER outside a conversation: said");
    reply = idle.fireTrigger("battery");
    check(decisionOf(reply, "battery") == vasdk::flow::DROPPED, "cooling down");

    // ask_first: yes runs the action (a flow, which comes back), no doesn't.
    auto asking = conversationAt(now);
    asking.start();
    reply = asking.fireTrigger("survey");
    check(reply.text == "Got a minute for a survey?" && reply.expectation.mode() == vasdk::flow::Expectation::YES_NO,
          "ask_first: asked, expecting yes or no");
    reply = asking.handleText("sure");
    check(reply.text == "Rate us. Book or hours?" && asking.pageName() == "welcome", "yes: the flow, then back");
    asking.fireTrigger("survey");
    reply = asking.handleText("no thanks");
    check(reply.text == "Okay. Book or hours?", "no: the question again");
    asking.fireTrigger("survey");
    reply = asking.handleText("hours");
    check(reply.text == "8 to 6. Book or hours?", "something else: the offer lapses and the turn goes on");

    // Quiet hours: suppressed, except CRITICAL.
    auto quietTime = now + std::chrono::minutes(150);
    auto quiet = conversationAt(quietTime);
    quiet.start();
    check(decisionOf(quiet.fireTrigger("promo"), "promo") == vasdk::flow::SUPPRESSED, "quiet hours: suppressed");
    check(decisionOf(quiet.fireTrigger("alarm"), "alarm") == vasdk::flow::STARTED, "CRITICAL during quiet hours");

    // The hourly limit.
    auto busy = conversationAt(now);
    busy.start();
    for (int i = 0; i < 6; ++i) busy.fireTrigger("promo");
    check(decisionOf(busy.fireTrigger("promo"), "promo") == vasdk::flow::DROPPED, "over max_per_hour: dropped");

    // Inactivity triggers, fired by name.
    const auto inactivity = conversation.inactivityTriggers();
    check(inactivity.size() == 1 && inactivity[0].first == "nudge" && inactivity[0].second == 30000, "inactivity triggers listed");

    // Load errors
    auto loadError = [&](const std::string& replace, const std::string& with) {
        std::string text = agentText(now);
        const auto at = text.find(replace);
        if (at == std::string::npos) return std::string("test bug");
        text.replace(at, replace.size(), with);
        std::string message;
        return compile(text, &message) ? std::string() : message;
    };
    check(loadError("start_flow: \"survey\"", "start_flow: \"surveys\"").find("unknown flow \"surveys\"") != std::string::npos,
          "an unknown flow");
    check(loadError("source { inactivity { after_ms: 30000 } }", "").find("has no source") != std::string::npos, "no source");
    check(loadError(hhmm(localMinuteOfDay(now) + 120), "25:99").find("HH:MM") != std::string::npos, "quiet hours must be HH:MM");

    if (failures == 0) std::printf("trigger_arbiter_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
