#pragma once

#include "Vertical.h"

#include <mutex>

namespace car {

/**
 * Text messages through the paired phone: an inbox read aloud, and messages sent from a form that
 * reads them back first. Its tool, `messages`, takes {"action", ...}: send ("to", "to_name",
 * "body"), read_latest (the oldest unread, now read), status. Who they're to is the phone's
 * @contact entity, so this vertical needs the phone's.
 */
class Messages : public Vertical {
public:
    explicit Messages(Car& car) : Vertical(car) {}
    std::string name() const override { return "messages"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    /** {"event": "message", "from": "Sam Carter", "body": "..."}: it arrives, and the conversation is told. */
    nlohmann::json simulate(const nlohmann::json& command) override;

    nlohmann::json command(const nlohmann::json& request);

private:
    struct Message { std::string from, body; bool read = false; bool sent = false; };
    nlohmann::json stateLocked() const;

    mutable std::mutex _mutex;
    std::vector<Message> _messages;   // in the order they came and went
};

} // namespace car
