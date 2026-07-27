#pragma once
#include "bridge_tool.h"

//  send_notification 
class SendNotificationTool : public BridgeTool {
public:
    explicit SendNotificationTool(AndroidToolBridge bridge)
        : BridgeTool("send_notification",
                     "Show a notification on the device. Only when the user explicitly asks for a notification or reminder",
                     std::move(bridge)) {
        addParameter({"title",      "Notification title",                                "string",  true});
        addParameter({"body",       "Notification body text",                            "string",  true});
        addParameter({"channel_id", "Notification channel ID (default: 'vasdk_alerts')", "string", false});
    }
};

//  set_alarm_or_timer
class SetAlarmOrTimerTool : public BridgeTool {
public:
    explicit SetAlarmOrTimerTool(AndroidToolBridge bridge)
        : BridgeTool("set_alarm_or_timer",
                     "Set an alarm (a specific clock time, e.g. 'wake me up at 7am') or a "
                     "countdown timer (a duration from now, e.g. 'in five seconds', 'a 10 minute "
                     "timer') in the clock app. Do NOT classify or convert the time yourself - "
                     "just copy it into 'when' as close to the user's own words as possible (e.g. "
                     "\"seven am\", \"5\", \"in 10 minutes\") and the app will work out whether "
                     "it's an alarm or a timer",
                     std::move(bridge)) {
        addParameter({"when",  "The requested time or duration, copied close to verbatim from the "
                                "user's words (e.g. '7am', 'five seconds', 'in 10 minutes')",
                                "string", true});
        addParameter({"label", "Optional label/message for the alarm or timer", "string", false});
    }
};
