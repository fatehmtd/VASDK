#pragma once

#include "Vertical.h"

#include <mutex>
#include <optional>

namespace car {

/**
 * The paired phone: calls in and out, and its contacts. Its tool, `phone`, takes {"action",
 * "value"}: answer, decline, hang_up, call (value: a contact), status. The contacts are the
 * agent's @contact entity, found in what's said by a code extractor ("contacts"): "call Sam",
 * "text doctor Patel". A call emits "call.started", and hanging up "call.ended", for media.
 */
class Phone : public Vertical {
public:
    explicit Phone(Car& car);
    std::string name() const override { return "phone"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    void registerExtensions(va::flow::FlowRegistry& registry) override;
    /** {"event": "incoming_call", "caller": "Mom"}: it rings, and the conversation is told. */
    nlohmann::json simulate(const nlohmann::json& command) override;

    nlohmann::json command(const nlohmann::json& request);

    struct Contact { std::string value, name, number; std::vector<std::string> aliases; };
    /** A contact by its value ("sam carter") or any of its names, as said; nullopt when unknown. */
    std::optional<Contact> contact(const std::string& said) const;

private:
    nlohmann::json stateLocked() const;
    std::vector<va::flow::FlowRegistry::EntityFound> findContacts(const std::vector<std::string>& words) const;

    mutable std::mutex _mutex;
    std::vector<Contact> _contacts;   // set once, read without the lock
    std::string _call = "idle";       // idle, ringing, active
    std::string _direction;           // incoming, outgoing
    std::string _caller;
};

} // namespace car
