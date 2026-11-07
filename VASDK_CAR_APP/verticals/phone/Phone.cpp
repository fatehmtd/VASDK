#include "Phone.h"

#include "Car.h"

#include <algorithm>

namespace car {

Phone::Phone(Car& car) : Vertical(car) {
    // shortcut: a fixed address book; a head unit reads the paired phone's (PBAP over Bluetooth).
    _contacts = {
        {"mom", "Mom", "555-0101", {"mom", "mum", "mother", "my mom"}},
        {"sam carter", "Sam Carter", "555-0142", {"sam", "sam carter", "carter"}},
        {"alex rivera", "Alex Rivera", "555-0177", {"alex", "alex rivera", "rivera"}},
        {"dr patel", "Dr. Patel", "555-0190", {"dr patel", "doctor patel", "patel", "my doctor"}},
        {"the office", "the office", "555-0100", {"the office", "office"}},
    };
}

std::optional<Phone::Contact> Phone::contact(const std::string& said) const {
    const auto wanted = words(said);
    for (const auto& contact : _contacts) {
        if (words(contact.value) == wanted || words(contact.name) == wanted) return contact;
        for (const auto& alias : contact.aliases)
            if (words(alias) == wanted) return contact;
    }
    return std::nullopt;
}

std::vector<va::flow::FlowRegistry::EntityFound> Phone::findContacts(const std::vector<std::string>& said) const {
    // The longest name at each place in the words: "call sam carter" is Sam Carter, not just "sam".
    std::vector<va::flow::FlowRegistry::EntityFound> found;
    for (size_t start = 0; start < said.size();) {
        size_t best = 0;
        const Contact* match = nullptr;
        for (const auto& contact : _contacts) {
            for (const auto& alias : contact.aliases) {
                const auto name = words(alias);
                if (name.size() <= best || start + name.size() > said.size()) continue;
                if (std::equal(name.begin(), name.end(), said.begin() + static_cast<long>(start))) {
                    best = name.size();
                    match = &contact;
                }
            }
        }
        if (!match) {
            ++start;
            continue;
        }
        va::flow::FlowRegistry::EntityFound entity;
        entity.start = start;
        entity.length = best;
        entity.value.set_string_value(match->value);
        entity.spoken = match->name;
        (*entity.attributes.mutable_fields())["number"].set_string_value(match->number);
        found.push_back(entity);
        start += best;
    }
    return found;
}

void Phone::registerExtensions(va::flow::FlowRegistry& registry) {
    registry.addEntityExtractor("contacts", [this](const va::flow::FlowRegistry::EntityRequest& request) {
        return findContacts(request.words);
    });
}

nlohmann::json Phone::stateLocked() const {
    nlohmann::json contacts = nlohmann::json::array();
    for (const auto& contact : _contacts) contacts.push_back(contact.name);
    return {{"state", _call}, {"caller", _caller}, {"direction", _direction}, {"contacts", contacts}};
}

nlohmann::json Phone::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return stateLocked();
}

nlohmann::json Phone::command(const nlohmann::json& request) {
    const auto action = lower(text(request.value("action", nlohmann::json())));
    const auto value = text(request.value("value", nlohmann::json()));
    std::string signal;   // emitted once the lock is let go
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        bool changedNow = true;
        if (action == "ring") {   // the simulator: a call comes in
            _caller = value.empty() ? "Someone" : value;
            if (const auto known = contact(_caller)) _caller = known->name;
            _call = "ringing";
            _direction = "incoming";
            signal = "call.started";   // the music stops while it rings
        } else if (action == "answer") {
            if (_call != "ringing") return error("nothing is ringing");
            _call = "active";
        } else if (action == "call") {
            const auto known = contact(value);
            if (!known) return error("no contact " + value);
            if (_call != "idle") return error("already on a call");
            _caller = known->name;
            _call = "active";
            _direction = "outgoing";
            signal = "call.started";
            result["number"] = known->number;
        } else if (action == "decline" || action == "hang_up") {
            changedNow = _call != "idle";
            if (changedNow) signal = "call.ended";
            _call = "idle";
            _direction.clear();
        } else if (action == "status" || action.empty()) {
            changedNow = false;
        } else {
            return error("unknown phone action: " + action);
        }
        result.update(stateLocked());
        result["changed"] = changedNow;
    }
    if (!signal.empty()) car().emit(signal, {{"caller", result["caller"]}});
    if (result.value("changed", false)) changed();
    return result;
}

std::vector<va::llm::Tool> Phone::tools() {
    va::llm::Tool phone("phone", "The driver's phone calls: answer, decline, hang_up, call a contact, status");
    phone.addParameter({"action", "answer, decline, hang_up, call (value: the contact's name) or status", "string", true});
    phone.addParameter({"value", "for call: a contact's name", "string", false});
    phone.onCall([this](const nlohmann::json& args) {
        if (lower(text(args.value("action", nlohmann::json()))) == "ring") return error("only the phone rings");
        return command(args);
    });
    return {phone};
}

nlohmann::json Phone::simulate(const nlohmann::json& request) {
    if (request.value("event", std::string()) != "incoming_call") return error("{\"event\": \"incoming_call\", \"caller\": \"Mom\"}");
    const auto rang = command({{"action", "ring"}, {"value", request.value("caller", std::string("Mom"))}});
    if (const auto conversation = car().conversation(); conversation.fireTrigger) {
        google::protobuf::Struct payload;
        (*payload.mutable_fields())["caller"].set_string_value(rang.value("caller", std::string()));
        conversation.fireTrigger("incoming-call", payload);
    }
    return rang;
}

} // namespace car
