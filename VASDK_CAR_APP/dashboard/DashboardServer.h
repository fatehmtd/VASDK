#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace car {

/**
 * The head unit's screen, in a browser: serves the dashboard page and keeps it live.
 *   GET  /          the page, and the files it loads (read from disk on each request, so they can be edited while running)
 *   GET  /state     the car's state, as JSON
 *   GET  /events    Server-Sent Events: {"type": "state"} when the car changes, and the conversation
 *                   ({"type": "user" | "assistant" | "hearing" | "agent" | "trigger", ...})
 *   POST /control   {"vertical": "climate", "action", "value", ...}: a touch
 *   POST /say       {"text"}: typed instead of spoken
 *   POST /simulate  {"vertical": "phone", "event": "incoming_call", ...}: the car's sensors and the world
 *   POST /stop      stops the assistant's reply
 *   GET  /info      {"verticals": [...], "webrtc": true}: the panels to load, and whether the page can talk to the car
 *   POST /offer     a WebRTC offer (SDP): the browser's mic and speaker become the car's; 409 while one is connected
 * Bound to this machine only.
 */
class DashboardServer {
public:
    struct Handlers {
        std::function<nlohmann::json()> state;
        std::function<nlohmann::json()> info;
        std::function<nlohmann::json(const nlohmann::json&)> control;
        std::function<void(const std::string&)> say;
        std::function<nlohmann::json(const nlohmann::json&)> simulate;
        std::function<void()> stop;
        /** WebRTC: the answer SDP for an offer, or empty with `status` set (409 busy, 400 unusable). Unset: no WebRTC. */
        std::function<std::string(const std::string& offer, int& status)> offer;
    };

    /** `files`: what's served, URL path → file ("/" → index.html, "/verticals/climate/panel.js" → ...). Nothing else is. */
    DashboardServer(std::map<std::string, std::string> files, Handlers handlers);
    ~DashboardServer();
    DashboardServer(const DashboardServer&) = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    /** Listens on host:port, on a thread of its own. False when the port can't be had. */
    bool start(const std::string& host, int port, std::string* error = nullptr);
    void stop();

    /** The car changed: sent to every page as the latest state (pages that fall behind skip to it). */
    void publishState(const nlohmann::json& state);
    /** A conversation event: kept (the last 100), so a page opened later shows what was said. */
    void publish(const nlohmann::json& event);
    /** What the user is saying, as heard so far: sent, not kept. */
    void publishHearing(const std::string& text);

private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace car
