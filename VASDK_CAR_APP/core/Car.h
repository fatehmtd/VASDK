#pragma once

#include "Vertical.h"

#include <VASDK/flow/FlowRegistry.h>

#include <google/protobuf/struct.pb.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace car {

/**
 * What the car may ask of the conversation, set by the app (VASDK_Car) from its session. Each is
 * optional: in tests by text there's no session, and a vertical does without.
 */
struct Conversation {
    std::function<void(const std::string& trigger, const google::protobuf::Struct& payload)> fireTrigger;
    std::function<void(const std::string& param, const google::protobuf::Value& value)> setSessionParam;
};

/**
 * The car: its verticals, and what joins them. Thread-safe: tools run on the conversation's
 * threads, touches on the dashboard's, ticks on the app's.
 */
class Car {
public:
    Car() = default;
    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;

    /** Adds a vertical, built with this car: car.add<Climate>(). */
    template <class V, class... Args>
    V& add(Args&&... args) {
        auto vertical = std::make_unique<V>(*this, std::forward<Args>(args)...);
        V& added = *vertical;
        _verticals.push_back(std::move(vertical));
        return added;
    }
    /** A vertical by name; nullptr when the car doesn't have it. */
    Vertical* find(const std::string& name) const;
    /** Its names, in the order added. */
    std::vector<std::string> names() const;

    /** Every vertical's state, by name: {"climate": {...}, "media": {...}}. */
    nlohmann::json state() const;
    /** One field of a vertical's state, or null: read("drive", "speed_kmh"). */
    nlohmann::json read(const std::string& vertical, const std::string& field) const;

    /** Every vertical's tools (the agent's and the LLM's) and extensions. */
    std::vector<va::llm::Tool> tools() const;
    void registerWith(va::flow::FlowRegistry& registry) const;

    /** A touch or a simulation, sent to the vertical named in {"vertical": ...}. */
    nlohmann::json control(const nlohmann::json& command);
    nlohmann::json simulate(const nlohmann::json& command);
    void tick(double seconds);

    //  Signals between verticals: "call.started", "call.ended", ...
    using Handler = std::function<void(const nlohmann::json& payload)>;
    void on(const std::string& signal, Handler handler);
    /** Calls each handler of `signal`, on this thread. Call it outside your own lock. */
    void emit(const std::string& signal, const nlohmann::json& payload = nlohmann::json::object());

    //  The car's changes, for the dashboard and $context.car
    void onChange(std::function<void(const nlohmann::json& state)> listener);
    void changed();

    void setConversation(Conversation conversation);
    Conversation conversation() const;

private:
    std::vector<std::unique_ptr<Vertical>> _verticals;   // added before use, never removed
    mutable std::mutex _mutex;
    std::map<std::string, std::vector<Handler>> _handlers;
    std::vector<std::function<void(const nlohmann::json&)>> _listeners;
    Conversation _conversation;
};

/** The car this app builds: every vertical in verticals/. One line each, here and in the agent's include_files. */
std::shared_ptr<Car> makeCar();

} // namespace car
