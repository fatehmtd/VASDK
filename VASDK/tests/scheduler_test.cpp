// Schedule and context triggers, on a fake clock: a daily reminder at 08:00, a one-off reminder
// the morning of a booking (its date from the session), one every 30 minutes, a low-battery
// warning when the battery drops (not while it stays low), the next due time for an app to wake
// for, state saved and restored so a reminder isn't repeated, and the dialog checking each second.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/DialogProcessor.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

using namespace va;
using namespace va::flow;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

std::chrono::system_clock::time_point localTime(int year, int month, int day, int hour, int minute) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

const char* kAgent = R"agent(
start_flow: "main"
session_params { name: "booking_day" type: STRING default { string_value: "2026-11-09" } }
triggers { name: "daily" source { schedule { at: "08:00" } } action { say { text: "Good morning." } } }
triggers { name: "booking" source { schedule { at: "${session.booking_day}T08:00" } } action { say { text: "Your booking is today." } } }
triggers { name: "half_hour" source { schedule { every_minutes: 30 } } action { say { text: "Half an hour." } } }
triggers { name: "battery" source { context_change { condition: "$context.battery < 15" } }
           action { say { text: "Battery low." } } }
flows { name: "main" start_page: "p" pages { name: "p" on_enter { ask { say { text: "Hi?" } } } } }
)agent";

std::string said(const Reply& reply) { return reply.text; }

} // namespace

int main() {
    vasdk::flow::Agent definition;
    (void)google::protobuf::TextFormat::ParseFromString(kAgent, &definition);
    std::string error;
    auto agent = CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;

    // A daily time, a one-off from the session, and every half hour.
    auto now = localTime(2026, 11, 8, 7, 30);
    double battery = 50;
    FlowRuntime conversation(agent);
    conversation.setClock([&now] { return now; });
    conversation.setContextSource([&battery] {
        google::protobuf::Struct context;
        (*context.mutable_fields())["battery"] = numberValue(battery);
        return context;
    });
    check(conversation.checkTriggers().triggers.empty(), "the first check fires nothing");
    check(conversation.nextScheduledTrigger() == localTime(2026, 11, 8, 8, 0), "the next due: 08:00 today");
    now = localTime(2026, 11, 8, 8, 0);
    auto reply = conversation.checkTriggers();
    check(said(reply).find("Good morning.") != std::string::npos && said(reply).find("Half an hour.") != std::string::npos,
          "08:00: the daily one, and half an hour since: " + said(reply));
    now = localTime(2026, 11, 8, 8, 10);
    check(conversation.checkTriggers().triggers.empty(), "08:10: nothing");
    now = localTime(2026, 11, 8, 8, 30);
    reply = conversation.checkTriggers();
    check(reply.text == "Half an hour.", "08:30: every half hour: " + said(reply));
    const std::string saved = conversation.triggerState();
    now = localTime(2026, 11, 9, 8, 0);
    reply = conversation.checkTriggers();
    check(said(reply).find("Your booking is today.") != std::string::npos && said(reply).find("Good morning.") != std::string::npos,
          "the morning of the booking: " + said(reply));
    now = localTime(2026, 11, 10, 8, 0);
    check(said(conversation.checkTriggers()).find("Your booking is today.") == std::string::npos, "the one-off doesn't come back");

    // Restored after a restart: what was due fires once, not again.
    FlowRuntime restarted(agent);
    now = localTime(2026, 11, 9, 8, 5);
    restarted.setClock([&now] { return now; });
    check(restarted.restoreTriggerState(saved), "the state restores");
    reply = restarted.checkTriggers();
    check(said(reply).find("Your booking is today.") != std::string::npos, "due while it was down: said once restored: " + said(reply));
    check(restarted.checkTriggers().triggers.empty(), "and not again");
    check(!restarted.restoreTriggerState("not json"), "bad state is refused");

    // A context condition: when it starts holding, not while it holds.
    battery = 40;
    conversation.checkTriggers();
    battery = 10;
    reply = conversation.checkTriggers();
    check(reply.text.rfind("Battery low.", 0) == 0, "the battery drops: warned: " + said(reply));
    battery = 8;
    check(conversation.checkTriggers().triggers.empty(), "still low: not again");
    battery = 60;
    conversation.checkTriggers();
    battery = 12;
    check(said(conversation.checkTriggers()).rfind("Battery low.", 0) == 0, "low again after charging: warned again");

    // The dialog checks each second.
    {
        std::atomic<double> level{50};
        auto dialog = std::make_shared<DialogProcessor>(agent);
        dialog->setContextSource([&level] {
            google::protobuf::Struct context;
            (*context.mutable_fields())["battery"] = numberValue(level.load());
            return context;
        });
        std::mutex mutex;
        std::vector<std::string> replies;
        std::vector<std::string> fired;   // the triggers that came due, said or queued
        dialog->setReplyObserver([&](const Reply& reply, const FlowRuntime&) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!reply.text.empty()) replies.push_back(reply.text);
            for (const auto& trigger : reply.triggers)
                if (trigger.decision == vasdk::flow::STARTED || trigger.decision == vasdk::flow::QUEUED) fired.push_back(trigger.name);
        });
        dialog->initialize();
        dialog->start();
        check(dialog->triggerState().empty() && !dialog->nextScheduledTrigger(), "nothing known before the first check");
        std::this_thread::sleep_for(1300ms);   // the first check sees 50
        level = 5;
        std::this_thread::sleep_for(1300ms);
        // For an app's alarm: the next due time and the state to keep, read from any thread.
        const auto next = dialog->nextScheduledTrigger();
        const auto inHalfAnHour = std::chrono::system_clock::now() + 30min;
        check(next && *next <= inHalfAnHour && *next > inHalfAnHour - 5s, "the next due time: in half an hour");
        check(dialog->triggerState().find("\"half_hour\"") != std::string::npos, "the triggers' state, to keep");
        // Last checked two hours ago, as if the app was closed since: the half-hour one is overdue.
        const auto twoHoursAgo = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() - 2h).time_since_epoch()).count();
        dialog->restoreTriggerState("{\"schedules\": {\"half_hour\": " + std::to_string(twoHoursAgo) + "}}");
        std::this_thread::sleep_for(1300ms);
        dialog->stop();
        std::lock_guard<std::mutex> lock(mutex);
        check(std::any_of(replies.begin(), replies.end(), [](const auto& text) { return text.rfind("Battery low.", 0) == 0; }),
              "the dialog's check warns about the battery");
        // (queued: the battery warning is still "playing", no audio here to finish it)
        check(std::find(fired.begin(), fired.end(), "half_hour") != fired.end(),
              "restored while running: what came due while closed fires at the next check");
    }

    // Load errors
    std::string text = kAgent;
    text.replace(text.find("every_minutes: 30"), std::string("every_minutes: 30").size(), "");
    vasdk::flow::Agent broken;
    (void)google::protobuf::TextFormat::ParseFromString(text, &broken);
    check(!CompiledAgent::compile(broken, &error) && error.find("schedule needs either at") != std::string::npos,
          "a schedule needs a time");

    if (failures == 0) std::printf("scheduler_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
