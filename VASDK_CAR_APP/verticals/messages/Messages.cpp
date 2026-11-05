#include "Messages.h"

#include "Car.h"

#include <algorithm>

namespace car {

nlohmann::json Messages::stateLocked() const {
    nlohmann::json list = nlohmann::json::array();
    int unread = 0;
    for (const auto& message : _messages) {
        list.push_back({{"from", message.from}, {"body", message.body}, {"read", message.read}, {"sent", message.sent}});
        if (!message.sent && !message.read) ++unread;
    }
    return {{"unread", unread}, {"messages", list}};
}

nlohmann::json Messages::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return stateLocked();
}

nlohmann::json Messages::command(const nlohmann::json& request) {
    const auto action = lower(text(request.value("action", nlohmann::json())));
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (action == "send") {
            const auto to = text(request.value("to_name", request.value("to", nlohmann::json())));
            const auto body = text(request.value("body", nlohmann::json()));
            if (to.empty() || body.empty()) return error("a message needs who it's to and what it says");
            // shortcut: kept here; a head unit hands it to the phone (MAP over Bluetooth)
            _messages.push_back({to, body, true, true});
            result = {{"sent", true}, {"to_name", to}, {"body", body}};
        } else if (action == "read_latest") {
            const auto unread = std::find_if(_messages.begin(), _messages.end(), [](const Message& m) { return !m.sent && !m.read; });
            if (unread == _messages.end()) {
                result = {{"found", false}};
            } else {
                unread->read = true;
                result = {{"found", true}, {"from", unread->from}, {"body", unread->body}};
            }
        } else if (action == "status" || action.empty()) {
            result = stateLocked();
            return result;
        } else {
            return error("unknown messages action: " + action);
        }
        result["unread"] = stateLocked()["unread"];
    }
    changed();
    return result;
}

std::vector<va::llm::Tool> Messages::tools() {
    va::llm::Tool messages("messages", "Text messages: send one, or read the next unread one");
    messages.addParameter({"action", "send, read_latest or status", "string", true});
    messages.addParameter({"to_name", "for send: the contact's name", "string", false});
    messages.addParameter({"body", "for send: what the message says", "string", false});
    messages.onCall([this](const nlohmann::json& args) { return command(args); });
    return {messages};
}

nlohmann::json Messages::simulate(const nlohmann::json& request) {
    if (request.value("event", std::string()) != "message") return error("{\"event\": \"message\", \"from\", \"body\"}");
    const auto from = request.value("from", std::string("Sam Carter"));
    const auto body = request.value("body", std::string("Running ten minutes late, sorry!"));
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _messages.push_back({from, body, false, false});
    }
    changed();
    if (const auto conversation = car().conversation(); conversation.fireTrigger) {
        google::protobuf::Struct payload;
        (*payload.mutable_fields())["from"].set_string_value(from);
        conversation.fireTrigger("message-received", payload);
    }
    return state();
}

} // namespace car
