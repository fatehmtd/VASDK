#pragma once

#include "Vertical.h"

#include <mutex>

namespace car {

/**
 * Driver profiles: who is driving (their key fob, in the simulator) and what they like: a
 * temperature, a genre, a volume, seat heating. When the driver changes, the conversation's
 * $session.driver is set and "driver-changed" fires; the agent applies the profile with the
 * climate's and media's tools. Its tool, `profile`, takes {"action"}: current, remember ("key":
 * temperature, genre, volume or seat_heat; "value").
 */
class Profiles : public Vertical {
public:
    explicit Profiles(Car& car);
    std::string name() const override { return "profiles"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    /** {"event": "key_fob", "driver": "Jordan"} */
    nlohmann::json simulate(const nlohmann::json& command) override;

    nlohmann::json command(const nlohmann::json& request);

private:
    struct Profile { std::string name; double temperature = 21; std::string genre; int volume = 12; int seatHeat = 0; };
    nlohmann::json json(const Profile& profile) const;
    void tellConversation(const nlohmann::json& driver) const;

    mutable std::mutex _mutex;
    std::vector<Profile> _profiles;
    size_t _current = 0;   // the guest, until a key fob says otherwise
};

} // namespace car
