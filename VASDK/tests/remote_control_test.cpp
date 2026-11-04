// Remote control over HTTP: the page; no token or a wrong one gets 401; a command in CONTROL
// mode drives the session and its reply arrives on the event stream; OBSERVE refuses commands
// (403) but streams events; without a token, it doesn't start, nor does the session.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/engine/RemoteControl.h>
#include <VASDK/flow/CompiledAgent.h>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "hours" examples: ["hours"] }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome" on_enter { ask { say { text: "How can I help?" } } }
            routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } } } }
)";

// Reads the event stream until `wanted` is in it, or the time is up.
bool streamShows(uint16_t port, const std::string& query, const std::string& wanted, const std::function<void()>& meanwhile) {
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5, 0);
    std::string seen;
    std::atomic<bool> found{false};
    std::thread reader([&] {
        client.Get("/events" + query, [&](const char* data, size_t length) {
            seen.append(data, length);
            if (seen.find(wanted) != std::string::npos) found = true;
            return !found.load();
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    meanwhile();
    for (int i = 0; i < 100 && !found; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.stop();
    reader.join();
    return found;
}

} // namespace

int main() {
    std::string error;
    vasdk::flow::Agent definition;
    engine::parseProto(kAgent, engine::ProtoFormat::Text, definition, &error);
    auto agent = flow::CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;

    auto tts = std::make_shared<fakes::FakeTts>();
    VAEngine::Options options;
    options.transport = std::make_shared<fakes::FakeTransport>();
    options.agent = agent;
    auto session = VAEngine::create(vasdk::engine::EngineConfig{}, fakes::makeRegistry(tts), options, &error);
    if (!session || !session->start(&error)) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }

    vasdk::engine::RemoteControl config;
    config.set_enabled(true);
    config.set_bind("127.0.0.1:0");
    check(!engine::RemoteControl::start(*session, config, &error) && error.find("token") != std::string::npos,
          "no token: it doesn't start");
    config.set_token("s3cret-token");
    config.set_mode(vasdk::engine::RemoteControl::CONTROL);
    auto remote = engine::RemoteControl::start(*session, config, &error);
    check(remote && remote->port() > 0, "it listens on a free port: " + error);
    if (!remote) return 1;
    httplib::Client client("127.0.0.1", remote->port());
    const httplib::Headers bearer{{"Authorization", "Bearer s3cret-token"}};

    auto page = client.Get("/");
    check(page && page->status == 200 && page->body.find("EventSource") != std::string::npos, "the trace page");
    auto noToken = client.Get("/events");
    check(noToken && noToken->status == 401, "events without the token: 401");
    auto wrong = client.Post("/command", httplib::Headers{{"Authorization", "Bearer guess"}}, R"({"injectText": "hours"})",
                             "application/json");
    check(wrong && wrong->status == 401, "a wrong token: 401");
    auto bad = client.Post("/command", bearer, R"({"nope": 1})", "application/json");
    check(bad && bad->status == 400, "not a command: 400");

    // A command drives the session; its reply arrives on the stream (the token in the address, as EventSource sends it).
    check(streamShows(remote->port(), "?token=s3cret-token", "We're open 8 to 6.", [&] {
              auto sent = client.Post("/command", bearer, R"({"injectText": "hours"})", "application/json");
              check(sent && sent->status == 200, "the command is taken");
          }),
          "its reply arrives on the event stream");

    // Observe only: events, no commands.
    remote.reset();
    config.set_mode(vasdk::engine::RemoteControl::OBSERVE);
    auto observer = engine::RemoteControl::start(*session, config, &error);
    check(observer != nullptr, "an observer starts: " + error);
    if (observer) {
        httplib::Client watcher("127.0.0.1", observer->port());
        auto refused = watcher.Post("/command", bearer, R"({"injectText": "hours"})", "application/json");
        check(refused && refused->status == 403, "observe only: commands get 403");
        check(streamShows(observer->port(), "?token=s3cret-token", "We're open 8 to 6.", [&] { session->injectUserText("hours"); }),
              "but events stream");
    }
    observer.reset();
    session->stop();

    // A session whose config enables it without a token doesn't start.
    vasdk::engine::EngineConfig withRemote;
    withRemote.mutable_remote_control()->set_enabled(true);
    VAEngine::Options second;
    second.transport = std::make_shared<fakes::FakeTransport>();
    second.agent = agent;
    auto unsafe = VAEngine::create(withRemote, fakes::makeRegistry(std::make_shared<fakes::FakeTts>()), second, &error);
    check(unsafe && !unsafe->start(&error) && error.find("token") != std::string::npos, "a session with remote_control and no token fails to start");

    if (failures == 0) std::printf("remote_control_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
