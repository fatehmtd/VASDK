#pragma once

#include <VASDK/flow/FlowRegistry.h>
#include <VASDK/llm/Tool.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace car {

class Car;

/**
 * One of the car's features, self-contained: climate, media, phone, navigation... A vertical
 * keeps its own state behind its own lock and offers it in three ways:
 *   - tools(): what the agent's call_tool steps and the LLM call (the same tools for both);
 *   - control(): the dashboard's touches;
 *   - simulate(): the dashboard's simulator, standing in for the car's sensors and the world
 *     (a call coming in, a message, the fuel gauge).
 * Its part of the conversation is an agent file next to its code (verticals/<name>/<name>.txtpb),
 * merged into the car's agent with include_files; its dashboard panel is panel.js beside it.
 *
 * Verticals don't reach into each other. They talk through the car's signals (Car::emit, Car::on):
 * the phone emits "call.started", and media pauses itself. They read each other's state as JSON
 * (Car::read), which is null when a vertical isn't there.
 */
class Vertical {
public:
    explicit Vertical(Car& car) : _car(car) {}
    virtual ~Vertical() = default;
    Vertical(const Vertical&) = delete;
    Vertical& operator=(const Vertical&) = delete;

    /** Its key in the car's state, in $context.car, in the dashboard and in control()'s commands. */
    virtual std::string name() const = 0;
    /** Its state, as JSON: what the dashboard draws and the agent's $context.car.<name> reads. */
    virtual nlohmann::json state() const = 0;

    /** Its tools, for call_tool and the LLM. */
    virtual std::vector<va::llm::Tool> tools() { return {}; }
    /** Anything else the agent needs from code: entity extractors, steps, envelopes. */
    virtual void registerExtensions(va::flow::FlowRegistry&) {}
    /** A dashboard touch: {"action", "value", ...}. Default: its first tool, with the command. */
    virtual nlohmann::json control(const nlohmann::json& command);
    /** The simulator. Default: nothing to simulate. */
    virtual nlohmann::json simulate(const nlohmann::json& command);
    /** Time moves on: the cabin warms, the car drives, the track plays. */
    virtual void tick(double /*seconds*/) {}

protected:
    Car& car() const { return _car; }
    /** Call after a change, outside the vertical's lock: the dashboard and $context.car follow. */
    void changed();

    static nlohmann::json error(const std::string& message) { return {{"error", message}}; }

private:
    Car& _car;
};

//  Reading what tools are given: values come from speech, the dashboard and LLMs, in any shape

std::string lower(std::string text);
/** A string, or anything else as JSON text; "" for null. */
std::string text(const nlohmann::json& value);
/** A number, or one written as text ("22"); `fallback` when there is none. */
double number(const nlohmann::json& value, double fallback);
/** "on", true, 1, "yes": true; "off", false, 0, "no": false; else `fallback`. */
bool truthy(const nlohmann::json& value, bool fallback);
/**
 * The action a tool is asked for, lower case, and its value. An LLM sometimes names the action as
 * the key: {"adjust_temperature": -1}. With no "action", a request whose only key isn't "value",
 * "zone" or one of `valueNames` (what the tool's value may also be called) gives the action, and
 * that key's value the value.
 */
std::string actionOf(const nlohmann::json& request, nlohmann::json& value, const std::vector<std::string>& valueNames);
/** The words of a name, lower case: "Dr. Patel" → {"dr", "patel"}. */
std::vector<std::string> words(const std::string& name);

} // namespace car
