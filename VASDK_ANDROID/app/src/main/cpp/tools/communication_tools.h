#pragma once
#include "bridge_tool.h"

//  get_contacts 
class GetContactsTool : public BridgeTool {
public:
    explicit GetContactsTool(AndroidToolBridge bridge)
        : BridgeTool("get_contacts",
                     "Search device contacts by name or phone number. Returns matching names with phone numbers and labels",
                     std::move(bridge)) {
        addParameter({"query", "Name (fuzzy match, typos okay) or phone number to search contacts for", "string", true});
    }
};

//  get_call_logs 
class GetCallLogsTool : public BridgeTool {
public:
    explicit GetCallLogsTool(AndroidToolBridge bridge)
        : BridgeTool("get_call_logs",
                     "Get recent call history. Each entry has: name (caller's name, or 'unknown' if not in "
                     "contacts), number (present ONLY when there is no name), type (incoming/outgoing/missed), "
                     "duration, date. When you answer, each entry MUST identify who the call was with - say the "
                     "name if present, otherwise say the number. Never answer with only the date and "
                     "type and no name or number - that omits who called, which is the whole point of asking. "
                     "No phone number or contact needed to call this - it always returns the device's own call log",
                     std::move(bridge)) {
        addParameter({"limit", "Maximum number of entries to return, defaults to 10 if omitted", "integer", false});
    }
};

//  make_phone_call 
class MakePhoneCallTool : public BridgeTool {
public:
    explicit MakePhoneCallTool(AndroidToolBridge bridge)
        : BridgeTool("make_phone_call",
                     "Place a phone call to a contact name or number. Only when the user explicitly asks to call someone. "
                     "Placing a call has real-world cost, so always confirm first: call this tool once without 'confirmed' "
                     "to resolve the contact, read the resolved name/number back to the user out loud, and only call it "
                     "again with confirmed=true after the user verbally agrees",
                     std::move(bridge)) {
        addParameter({"contact", "Contact name (e.g. \"Mom\", \"Dr. Smith\") or a phone number to call", "string", true});
        addParameter({"confirmed", "Set to true only after the user has verbally confirmed the resolved contact/number. Omit or leave false on the first call", "boolean", false});
    }
};

//  get_calendar_events 
class GetCalendarEventsTool : public BridgeTool {
public:
    explicit GetCalendarEventsTool(AndroidToolBridge bridge)
        : BridgeTool("get_calendar_events",
                     "Get upcoming calendar events: title, start, end, location. "
                     "No title or time needed to call this - it only reads existing events",
                     std::move(bridge)) {
        addParameter({"days_ahead", "How many days ahead to look for events, defaults to 7 if omitted", "integer", false});
    }
};

//  add_calendar_event 
class AddCalendarEventTool : public BridgeTool {
public:
    explicit AddCalendarEventTool(AndroidToolBridge bridge)
        : BridgeTool("add_calendar_event",
                     "Add an event to the calendar. Only when the user explicitly asks to schedule something",
                     std::move(bridge)) {
        addParameter({"title",       "Required. Short event name shown on the calendar, e.g. \"Dentist appointment\". Always provide this, even if the user only gave a description.", "string", true});
        addParameter({"start_time",  "Start time in ISO 8601 format (e.g. 2025-06-01T14:00:00). Optional: if omitted, the event is created as an all-day event for today. To make an all-day event on a different day, still pass that day's date as start_time.", "string", false});
        addParameter({"end_time",    "Optional. End time in ISO 8601 format. If omitted, defaults to one hour after start_time.", "string", false});
        addParameter({"description", "Optional additional details beyond the title, e.g. notes or agenda. Do not put the event name here instead of in title.", "string", false});
        addParameter({"location",    "Optional event location",                      "string", false});
    }
};

//  send_sms 
class SendSmsTool : public BridgeTool {
public:
    explicit SendSmsTool(AndroidToolBridge bridge)
        : BridgeTool("send_sms",
                     "Send an SMS to a contact name or number. Only when the user explicitly asks to send a text message. "
                     "Sending a text has real-world cost, so always confirm first: call this tool once without 'confirmed' "
                     "to resolve the contact, read the recipient and message content back to the user out loud, and only "
                     "call it again with confirmed=true after the user verbally agrees",
                     std::move(bridge)) {
        addParameter({"contact",   "Contact name (e.g. \"John\") or phone number of the recipient", "string",  true});
        addParameter({"message",   "Text message content",                                          "string",  true});
        addParameter({"confirmed", "Set to true only after the user has verbally confirmed the recipient and message content. Omit or leave false on the first call", "boolean", false});
    }
};
