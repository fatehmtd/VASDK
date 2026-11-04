#include "engine/RemoteControl.h"

#include "engine/Session.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_HTTPLIB
#include <httplib.h>
#endif

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace va {
namespace engine {

namespace {

constexpr size_t kKeptEvents = 512;   // what a client that connects late, or falls behind, can still read

// The trace page: the token comes in its own address (?token=...), and goes with each request.
constexpr const char* kPage = R"html(<!doctype html>
<html><head><meta charset="utf-8"><title>VASDK session</title>
<style>
body { font: 14px system-ui, sans-serif; margin: 0; display: flex; flex-direction: column; height: 100vh; }
header { padding: 8px 12px; background: #1f2937; color: #fff; }
#trace { flex: 1; overflow: auto; padding: 8px 12px; font-family: ui-monospace, monospace; }
.kind { color: #2563eb; } .you { color: #047857; } .bot { color: #7c3aed; } .error { color: #b91c1c; }
form { display: flex; gap: 8px; padding: 8px 12px; border-top: 1px solid #e5e7eb; }
input { flex: 1; padding: 6px; }
</style></head><body>
<header>VASDK session <span id="status">connecting...</span></header>
<div id="trace" aria-live="polite"></div>
<form id="say"><label for="text">Type as the user:</label><input id="text" autocomplete="off"><button>Send</button></form>
<script>
const token = new URLSearchParams(location.search).get("token") || "";
const trace = document.getElementById("trace");
function line(kind, text, style) {
  const row = document.createElement("div");
  row.innerHTML = '<span class="kind"></span> <span></span>';
  row.children[0].textContent = "[" + kind + "]";
  row.children[1].textContent = text;
  if (style) row.children[1].className = style;
  trace.appendChild(row);
  trace.scrollTop = trace.scrollHeight;
}
function describe(event) {
  if (event.transcript) return event.transcript.isFinal ? ["you", event.transcript.text, "you"] : null;
  if (event.agentReply) return event.agentReply.isFinal ? ["assistant", event.agentReply.text, "bot"] : null;
  if (event.pageChanged) { const p = event.pageChanged;
    return ["page", (p.fromFlow || "") + "/" + (p.fromPage || "") + " → " + p.toFlow + "/" + p.toPage + " (" + p.reason + ")"]; }
  if (event.agentState) return ["state", event.agentState.previousState + " → " + event.agentState.state];
  if (event.error) return ["error", event.error.message, "error"];
  const kind = Object.keys(event).find(key => key !== "timestampMs" && key !== "sessionId");
  return kind ? [kind, JSON.stringify(event[kind])] : null;
}
const events = new EventSource("events?token=" + encodeURIComponent(token));
events.onopen = () => document.getElementById("status").textContent = "live";
events.onerror = () => document.getElementById("status").textContent = "disconnected (is the token right?)";
events.onmessage = message => { const shown = describe(JSON.parse(message.data)); if (shown) line(...shown); };
document.getElementById("say").onsubmit = async submit => {
  submit.preventDefault();
  const text = document.getElementById("text");
  const answer = await fetch("command", { method: "POST", headers: { "Authorization": "Bearer " + token },
                                          body: JSON.stringify({ injectText: text.value }) });
  if (!answer.ok) line("command", (await answer.json()).error || answer.statusText, "error");
  text.value = "";
};
</script></body></html>)html";

// The same length and bytes, compared in a time that doesn't depend on where they differ.
bool sameToken(const std::string& given, const std::string& expected) {
    if (given.size() != expected.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < given.size(); ++i) difference |= static_cast<unsigned char>(given[i] ^ expected[i]);
    return difference == 0;
}

std::string errorJson(const std::string& message) {
    std::string quoted;
    for (const char c : message) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    return "{\"error\": \"" + quoted + "\"}";
}

} // namespace

struct RemoteControl::State {
    State(Session& running, vasdk::engine::RemoteControl settings) : session(running), config(std::move(settings)) {}
    Session& session;
    vasdk::engine::RemoteControl config;
    int listenerId = -1;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::pair<uint64_t, std::string>> recent;   // event id, its JSON
    uint64_t nextId = 1;
    bool stopping = false;
    int port = 0;
#ifdef VASDK_HAS_HTTPLIB
    httplib::Server server;
    std::thread thread;
#endif
};

RemoteControl::RemoteControl(std::unique_ptr<State> state) : _state(std::move(state)) {}

RemoteControl::~RemoteControl() {
    if (_state->listenerId >= 0) _state->session.removeListener(_state->listenerId);
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->stopping = true;
    }
    _state->changed.notify_all();
#ifdef VASDK_HAS_HTTPLIB
    _state->server.stop();
    if (_state->thread.joinable()) _state->thread.join();
#endif
}

uint16_t RemoteControl::port() const { return static_cast<uint16_t>(_state->port); }

bool RemoteControl::handle(const vasdk::engine::Command& command, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (_state->config.mode() != vasdk::engine::RemoteControl::CONTROL) return fail("observe only: this session takes no commands");
    auto& session = _state->session;
    switch (command.kind_case()) {
    case vasdk::engine::Command::kInjectText: session.injectUserText(command.inject_text()); break;
    case vasdk::engine::Command::kSelectChoice: session.selectChoice(command.select_choice()); break;
    case vasdk::engine::Command::kFireTrigger: session.fireTrigger(command.fire_trigger().name(), command.fire_trigger().payload()); break;
    case vasdk::engine::Command::kSay: session.say(command.say()); break;
    case vasdk::engine::Command::kInterrupt: session.interrupt(); break;
    case vasdk::engine::Command::kSetSessionParam:
        session.setSessionParam(command.set_session_param().name(), command.set_session_param().value());
        break;
    case vasdk::engine::Command::KIND_NOT_SET: return fail("an empty command");
    }
    return true;
}

std::unique_ptr<RemoteControl> RemoteControl::start(Session& session, const vasdk::engine::RemoteControl& config, std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<RemoteControl> {
        if (error) *error = "remote_control: " + message;
        return nullptr;
    };
    if (config.token().empty()) return fail("needs a token (token: \"${VASDK_RC_TOKEN}\")");
#ifndef VASDK_HAS_HTTPLIB
    (void)session;
    return fail("this build has no HTTP server (VASDK_ENABLE_WEBSOCKET_SERVER)");
#else
    const std::string bind = config.bind().empty() ? "127.0.0.1:8765" : config.bind();
    const auto colon = bind.rfind(':');
    if (colon == std::string::npos || colon == 0) return fail("bind \"" + bind + "\" isn't host:port");
    const std::string host = bind.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(bind.substr(colon + 1));
    } catch (...) {
        return fail("bind \"" + bind + "\" has no port");
    }

    std::unique_ptr<RemoteControl> remote(new RemoteControl(std::make_unique<State>(session, config)));
    State* state = remote->_state.get();
    auto authorized = [state](const httplib::Request& request) {
        const auto header = request.get_header_value("Authorization");
        const std::string given = header.rfind("Bearer ", 0) == 0 ? header.substr(7) : request.get_param_value("token");
        return sameToken(given, state->config.token());
    };

    state->server.Get("/", [](const httplib::Request&, httplib::Response& response) { response.set_content(kPage, "text/html"); });
    state->server.Get("/events", [state, authorized](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request)) {
            response.status = 401;
            response.set_content(errorJson("unauthorized"), "application/json");
            return;
        }
        auto cursor = std::make_shared<uint64_t>(0);   // the next event this stream sends; 0: the first kept
        response.set_header("Cache-Control", "no-cache");
        response.set_chunked_content_provider("text/event-stream", [state, cursor](size_t, httplib::DataSink& sink) {
            std::unique_lock<std::mutex> lock(state->mutex);
            const bool woken = state->changed.wait_for(lock, std::chrono::seconds(15), [&] {
                return state->stopping || (!state->recent.empty() && state->recent.back().first >= *cursor);
            });
            if (state->stopping) return false;
            std::string out = woken ? "" : ": still here\n\n";   // shows a closed connection, keeps proxies from closing it
            for (const auto& [id, json] : state->recent) {
                if (id < *cursor) continue;
                out += "id: " + std::to_string(id) + "\ndata: " + json + "\n\n";
                *cursor = id + 1;
            }
            lock.unlock();
            return sink.write(out.data(), out.size());
        });
    });
    state->server.Post("/command", [remote = remote.get(), authorized](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request)) {
            response.status = 401;
            response.set_content(errorJson("unauthorized"), "application/json");
            return;
        }
        vasdk::engine::Command command;
        if (!google::protobuf::util::JsonStringToMessage(request.body, &command).ok()) {
            response.status = 400;
            response.set_content(errorJson("not a Command: {\"injectText\": \"...\"}, {\"selectChoice\": 0}, ..."), "application/json");
            return;
        }
        std::string reason;
        if (!remote->handle(command, &reason)) {
            response.status = reason.rfind("observe only", 0) == 0 ? 403 : 400;
            response.set_content(errorJson(reason), "application/json");
            return;
        }
        response.set_content("{\"ok\": true}", "application/json");
    });

    if (port == 0) {
        state->port = state->server.bind_to_any_port(host);
        if (state->port <= 0) return fail("can't listen on " + host);
    } else if (!state->server.bind_to_port(host, port)) {
        return fail("can't listen on " + bind + " (in use?)");
    } else {
        state->port = port;
    }
    state->listenerId = session.addListener([state](const vasdk::engine::Event& event) {
        std::string json;
        if (!google::protobuf::util::MessageToJsonString(event, &json).ok()) return;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->recent.emplace_back(state->nextId++, std::move(json));
            while (state->recent.size() > kKeptEvents) state->recent.pop_front();
        }
        state->changed.notify_all();
    });
    state->thread = std::thread([state] { state->server.listen_after_bind(); });
    state->server.wait_until_ready();
    spdlog::info("[RemoteControl] {} on http://{}:{}", config.mode() == vasdk::engine::RemoteControl::CONTROL ? "control" : "observe",
                 host, state->port);
    return remote;
#endif
}

} // namespace engine
} // namespace va
