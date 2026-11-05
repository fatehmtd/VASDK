#include "DashboardServer.h"

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace car {

struct DashboardServer::State {
    std::map<std::string, std::string> files;
    Handlers handlers;
    httplib::Server server;
    std::thread thread;

    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::deque<std::pair<uint64_t, std::string>> recent;   // conversation events, by id
    uint64_t nextId = 1;
    const size_t keep = 100;
    std::string latestState;   // the car, as JSON
    uint64_t stateVersion = 0;
    std::string hearing;       // the partial transcript
    uint64_t hearingVersion = 0;
};

namespace {

std::string errorJson(const std::string& message) { return nlohmann::json{{"error", message}}.dump(); }

nlohmann::json body(const httplib::Request& request) {
    return nlohmann::json::parse(request.body, nullptr, false);
}

} // namespace

DashboardServer::DashboardServer(std::map<std::string, std::string> files, Handlers handlers) : _state(std::make_shared<State>()) {
    _state->files = std::move(files);
    _state->handlers = std::move(handlers);
}

DashboardServer::~DashboardServer() { stop(); }

bool DashboardServer::start(const std::string& host, int port, std::string* error) {
    auto state = _state;
    auto& server = state->server;

    for (const auto& [url, path] : state->files) {   // only these: no path from a request reaches the disk
        server.Get(url, [path = path](const httplib::Request&, httplib::Response& response) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                response.status = 404;
                response.set_content("not found: " + path, "text/plain");
                return;
            }
            std::stringstream content;
            content << file.rdbuf();
            const auto ext = std::filesystem::path(path).extension().string();
            const char* type = ext == ".js" ? "text/javascript; charset=utf-8" : ext == ".css" ? "text/css; charset=utf-8"
                                                                                              : "text/html; charset=utf-8";
            response.set_header("Cache-Control", "no-cache");
            response.set_content(content.str(), type);
        });
    }
    server.Get("/state", [state](const httplib::Request&, httplib::Response& response) {
        response.set_content(state->handlers.state().dump(), "application/json");
    });
    server.Get("/events", [state](const httplib::Request&, httplib::Response& response) {
        // Each stream: the kept conversation, then what comes; the car's state whenever it changed.
        struct Cursor { uint64_t next = 0; uint64_t stateVersion = 0; uint64_t hearingVersion = 0; };
        auto cursor = std::make_shared<Cursor>();
        response.set_header("Cache-Control", "no-cache");
        response.set_chunked_content_provider("text/event-stream", [state, cursor](size_t, httplib::DataSink& sink) {
            std::unique_lock<std::mutex> lock(state->mutex);
            const bool woken = state->changed.wait_for(lock, std::chrono::seconds(15), [&] {
                return state->stopping || (!state->recent.empty() && state->recent.back().first >= cursor->next) ||
                       state->stateVersion != cursor->stateVersion || state->hearingVersion != cursor->hearingVersion;
            });
            if (state->stopping) return false;
            std::string out = woken ? "" : ": still here\n\n";
            for (const auto& [id, json] : state->recent) {
                if (id < cursor->next) continue;
                out += "data: " + json + "\n\n";
                cursor->next = id + 1;
            }
            if (state->stateVersion != cursor->stateVersion) {
                out += "data: {\"type\":\"state\",\"state\":" + state->latestState + "}\n\n";
                cursor->stateVersion = state->stateVersion;
            }
            if (state->hearingVersion != cursor->hearingVersion) {
                out += "data: " + nlohmann::json{{"type", "hearing"}, {"text", state->hearing}}.dump() + "\n\n";
                cursor->hearingVersion = state->hearingVersion;
            }
            lock.unlock();
            return sink.write(out.data(), out.size());
        });
    });
    server.Post("/control", [state](const httplib::Request& request, httplib::Response& response) {
        const auto command = body(request);
        if (!command.is_object()) {
            response.status = 400;
            response.set_content(errorJson("not JSON: {\"vertical\", \"action\", \"value\"}"), "application/json");
            return;
        }
        const auto result = state->handlers.control(command);
        if (result.contains("error")) response.status = 400;
        response.set_content(result.dump(), "application/json");
    });
    server.Post("/say", [state](const httplib::Request& request, httplib::Response& response) {
        const auto command = body(request);
        const auto text = command.is_object() ? command.value("text", std::string()) : std::string();
        if (text.empty()) {
            response.status = 400;
            response.set_content(errorJson("{\"text\": \"...\"}"), "application/json");
            return;
        }
        state->handlers.say(text);
        response.set_content("{\"ok\": true}", "application/json");
    });
    server.Post("/simulate", [state](const httplib::Request& request, httplib::Response& response) {
        const auto command = body(request);
        if (!command.is_object()) {
            response.status = 400;
            response.set_content(errorJson("not JSON"), "application/json");
            return;
        }
        const auto result = state->handlers.simulate(command);
        if (result.contains("error")) response.status = 400;
        response.set_content(result.dump(), "application/json");
    });
    server.Get("/info", [state](const httplib::Request&, httplib::Response& response) {
        auto info = state->handlers.info ? state->handlers.info() : nlohmann::json::object();
        info["webrtc"] = static_cast<bool>(state->handlers.offer);
        response.set_content(info.dump(), "application/json");
    });
    server.Post("/offer", [state](const httplib::Request& request, httplib::Response& response) {
        if (!state->handlers.offer) {
            response.status = 404;
            return;
        }
        int status = 200;
        const auto answer = state->handlers.offer(request.body, status);   // blocks while ICE gathers
        if (answer.empty()) {
            response.status = status == 200 ? 400 : status;
            return;
        }
        response.set_content(answer, "application/sdp");
    });
    server.Post("/stop", [state](const httplib::Request&, httplib::Response& response) {
        state->handlers.stop();
        response.set_content("{\"ok\": true}", "application/json");
    });

    if (!server.bind_to_port(host, port)) {
        if (error) *error = "can't listen on " + host + ":" + std::to_string(port);
        return false;
    }
    state->thread = std::thread([state] { state->server.listen_after_bind(); });
    return true;
}

void DashboardServer::stop() {
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (_state->stopping) return;
        _state->stopping = true;
    }
    _state->changed.notify_all();
    _state->server.stop();
    if (_state->thread.joinable()) _state->thread.join();
}

void DashboardServer::publishState(const nlohmann::json& state) {
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->latestState = state.dump();
        ++_state->stateVersion;
    }
    _state->changed.notify_all();
}

void DashboardServer::publish(const nlohmann::json& event) {
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->recent.emplace_back(_state->nextId++, event.dump());
        while (_state->recent.size() > _state->keep) _state->recent.pop_front();
    }
    _state->changed.notify_all();
}

void DashboardServer::publishHearing(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->hearing = text;
        ++_state->hearingVersion;
    }
    _state->changed.notify_all();
}

} // namespace car
