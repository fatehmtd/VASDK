// Tools and security: call_tool with a filler, results stored, failures raising tool-error; HTTP
// tools only reach allow-listed URLs, with secrets from the engine config and capped replies;
// agent files can't hold secrets; sensitive values are masked in events; step outcomes from
// other languages may only write what a set step could.

#include <VASDK/engine/EventTapProcessor.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/network/HttpInterface.h>

#include "vasdk/engine/events.pb.h"

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace va;
using namespace va::flow;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

template <class Message>
Message parse(const std::string& text) {
    Message message;
    if (!google::protobuf::TextFormat::ParseFromString(text, &message)) std::fprintf(stderr, "parse error\n");
    return message;
}

// Records requests; answers with `reply` and `status`.
class FakeHttp : public network::HttpInterface {
public:
    network::HttpResponse post(const std::string& url, const std::map<std::string, std::string>& headers,
                               const std::string& body, int, std::function<void(const std::string&)>) override {
        return answer("POST " + url, headers, body);
    }
    network::HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers, int) override {
        return answer("GET " + url, headers, "");
    }
    bool sendText(const std::string&, const std::map<std::string, std::string>&, const std::string&, int) override { return false; }
    bool sendBytes(const std::string&, const std::map<std::string, std::string>&, const std::vector<uint8_t>&, int) override { return false; }

    network::HttpResponse answer(std::string request, const std::map<std::string, std::string>& headers, const std::string& body) {
        requests.push_back(std::move(request));
        lastHeaders = headers;
        lastBody = body;
        network::HttpResponse response;
        response.statusCode = status;
        response.body = reply;
        return response;
    }
    std::vector<std::string> requests;
    std::map<std::string, std::string> lastHeaders;
    std::string lastBody;
    int status = 200;
    std::string reply = R"({"id": "B-42"})";
};

const char* kAgent = R"agent(
start_flow: "main"
session_params { name: "booking" type: STRUCT }
session_params { name: "card" type: STRING sensitive: true }
intents { name: "book" examples: ["book"] }
intents { name: "slow" examples: ["slow"] }
intents { name: "lookup" examples: ["lookup"] }
tools { name: "create_booking" http { url: "https://api.acme.example/bookings"
        headers { key: "Authorization" value: "Bearer $secrets.acme" } } }
tools { name: "find_slots" http { url: "https://api.acme.example/slots" method: "GET" } }
tools { name: "elsewhere" http { url: "https://api.acme.example.evil.example/steal" } }
goals { name: "pay" slots { name: "phone" type: PHONE sensitive: true } }
flows { name: "main" start_page: "start"
  pages { name: "start"
    on_enter { ask { say { text: "Hi." } } }
    routes { intent: "book"
             steps { say { text: "Sure." } }
             steps { call_tool { name: "create_booking" args { key: "day" value: "'tuesday'" } into: "$session.booking"
                                 filler: "One moment." } }
             steps { say { text: "Booked $session.booking.id." } }
             target: "done" }
    routes { intent: "slow" steps { call_tool { name: "sleepy" } } target: "done" }
    routes { intent: "lookup" steps { call_tool { name: "find_slots" args { key: "day" value: "'friday'" } into: "$session.booking" } }
             steps { say { text: "Found $session.booking.id." } } }
    handlers { event: "tool-error" steps { say { text: "Our system is down." } } }
  }
  pages { name: "done" on_enter { say { text: "Bye." } } on_enter { end {} } }
  pages { name: "pay" form { goal: "pay" } routes { condition: "$form.complete" target: "done" } }
}
)agent";

std::shared_ptr<FlowRegistry> registry() {
    auto registry = std::make_shared<FlowRegistry>();
    llm::Tool sleepy("sleepy", "Never answers in time");
    sleepy.onCall([](const nlohmann::json&) -> nlohmann::json {
        std::this_thread::sleep_for(500ms);
        return {{"ok", true}};
    });
    registry->addTool(std::move(sleepy));
    return registry;
}

std::string replaced(std::string text, const std::string& replace, const std::string& with) {
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    return text.replace(at, replace.size(), with);
}

std::string compileError(const std::string& replace, const std::string& with) {
    std::string error;
    return CompiledAgent::compile(parse<vasdk::flow::Agent>(replaced(kAgent, replace, with)), &error, nullptr, registry())
               ? "" : error;
}

ToolResult failing(const std::string&, const nlohmann::json&) {
    ToolResult result;
    result.error = "down";
    return result;
}

} // namespace

int main() {
    // The allow-list: * matches anything; no user info, no other hosts, no plain http unless listed.
    google::protobuf::RepeatedPtrField<std::string> allow;
    allow.Add("https://api.acme.example/*");
    check(ToolRunner::allowed(allow, "https://api.acme.example/bookings?day=1"), "an allowed URL");
    check(!ToolRunner::allowed(allow, "https://api.acme.example.evil.example/x"), "a longer host isn't allowed");
    check(!ToolRunner::allowed(allow, "https://api.acme.example@evil.example/x"), "user info can't smuggle a host");
    check(!ToolRunner::allowed(allow, "http://api.acme.example/bookings"), "plain http isn't https");
    check(!ToolRunner::allowed(allow, "https://api.acme.example/a b"), "no spaces");

    std::string error;
    auto agent = CompiledAgent::compile(parse<vasdk::flow::Agent>(kAgent), &error, nullptr, registry());
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    vasdk::engine::Security security;
    security.add_http_allow("https://api.acme.example/*");
    (*security.mutable_secrets())["acme"] = "s3cr3t-token";
    security.set_timeout_ms(100);
    security.set_max_response_kb(1);
    auto http = std::make_shared<FakeHttp>();
    auto tools = std::make_shared<ToolRunner>(agent, security, http);
    auto conversationWithTools = [&](std::shared_ptr<const CompiledAgent> which) {
        FlowRuntime conversation(std::move(which));
        conversation.setToolCaller(ToolRunner::caller(tools));
        conversation.start();
        return conversation;
    };

    // call_tool: the filler is heard first, the result is stored, the steps after it run.
    {
        auto conversation = conversationWithTools(agent);
        std::vector<std::string> heard;
        conversation.setFillerSink([&](const Reply& sofar) { heard.push_back(sofar.text); });
        auto reply = conversation.handleText("book");
        check(heard == std::vector<std::string>{"Sure. One moment."}, "the filler is said before the call");
        check(reply.text == "Booked B-42. Bye." && reply.endsSession, "the result stored and read; the route's target");
        check(http->requests == std::vector<std::string>{"POST https://api.acme.example/bookings"}, "a POST to the tool's URL");
        check(http->lastHeaders["Authorization"] == "Bearer s3cr3t-token", "the secret from the engine config");
        check(nlohmann::json::parse(http->lastBody) == nlohmann::json{{"day", "tuesday"}}, "the args as a JSON body");
        check(reply.toolCalls.size() == 1 && reply.toolCalls[0].ok && contains(reply.toolCalls[0].resultJson, "B-42"),
              "the call is reported");
    }
    // Without a filler sink (tests, agentc), the filler is part of the reply.
    check(conversationWithTools(agent).handleText("book").text == "Sure. One moment. Booked B-42. Bye.", "the filler in the reply");
    // A GET with the args as a query.
    {
        http->reply = R"({"id": "S-7"})";
        auto reply = conversationWithTools(agent).handleText("lookup");
        check(http->requests.back() == "GET https://api.acme.example/slots?day=friday", "a GET with a query");
        check(contains(reply.text, "Found S-7."), "the GET's result");
    }
    // Failures raise tool-error: a long reply, a server error, a timeout. The route's target isn't
    // taken; the handler runs and the question is asked again.
    {
        auto failsWith = [&](const std::string& what) {
            auto conversation = conversationWithTools(agent);
            auto reply = conversation.handleText(what);
            const bool ok = reply.event == kEventToolError && contains(reply.text, "Our system is down. Hi.") && !contains(reply.text, "Bye") &&
                            conversation.pageName() == "start" && !reply.toolCalls.empty() && !reply.toolCalls[0].ok;
            if (!ok) std::fprintf(stderr, "  %s → %s\n", what.c_str(), reply.text.c_str());
            return ok;
        };
        http->reply = std::string(2000, 'x');
        check(failsWith("book"), "a reply over max_response_kb fails");
        http->reply = "{}";
        http->status = 500;
        check(failsWith("book"), "HTTP 500 fails");
        http->status = 200;
        const auto started = std::chrono::steady_clock::now();
        check(failsWith("slow"), "a C++ tool past timeout_ms fails");
        check(std::chrono::steady_clock::now() - started < 400ms, "without waiting for it");

        const auto before = http->requests.size();
        auto result = tools->call("elsewhere", nlohmann::json::object());
        check(!result.ok && contains(result.error, "http_allow") && http->requests.size() == before,
              "a URL off the allow-list is never requested");
        vasdk::engine::Security noSecret = security;
        noSecret.mutable_secrets()->clear();
        result = ToolRunner(agent, noSecret, http).call("create_booking", nlohmann::json::object());
        check(!result.ok && contains(result.error, "no secret \"acme\""), "a missing secret fails the call");
        result = ToolRunner(agent, {}, http).call("create_booking", nlohmann::json::object());
        check(!result.ok && contains(result.error, "http_allow"), "with no engine config, no HTTP");
    }
    // Without a tool-error handler: the built-in reply and the question again.
    {
        auto plain = CompiledAgent::compile(
            parse<vasdk::flow::Agent>(replaced(kAgent, "handlers { event: \"tool-error\" steps { say { text: \"Our system is down.\" } } }", "")),
            &error, nullptr, registry());
        FlowRuntime conversation(plain);
        conversation.setToolCaller(failing);
        conversation.start();
        check(conversation.handleText("book").text == "Sure. One moment. Sorry, I can't do that right now. Hi.", "built-in tool-error");
    }

    // Agent files can't hold secrets.
    check(contains(compileError("text: \"Hi.\"", "text: \"Hi sk-abcdefghijklmnopqrstuvwxyz123\""), "looks like a secret"),
          "a key-like literal");
    check(contains(compileError("text: \"Hi.\"", "text: \"Hi ${HOME}\""), "reads the environment"), "${ENV} in an agent");
    check(contains(compileError("text: \"Hi.\"", "text: \"Hi $secrets.acme\""), "only read in a tool's headers"), "$secrets in a say");
    check(contains(compileError("value: \"Bearer $secrets.acme\"", "value: \"Bearer abc\""), "holds a literal"),
          "a literal Authorization header");
    check(contains(compileError("name: \"sleepy\"", "name: \"sleepyy\""), "unknown tool \"sleepyy\""), "an unknown tool");

    // Sensitive values: masked as receipts do, and listed on every reply for events and logs.
    check(maskValue("555-123-4567") == "***-***-4567", "a phone number keeps its last 4 digits");
    check(maskValue("1234") == "****" && maskValue("Sam") == "***", "short values and words are all masked");
    check(redact("call 555-123-4567 now", {"555-123-4567"}) == "call ***-***-4567 now", "redact");
    {
        FlowRuntime conversation(agent);
        conversation.setSessionParam("card", stringValue("4111111111111111"));
        auto reply = conversation.start();
        check(std::find(reply.sensitiveValues.begin(), reply.sensitiveValues.end(), "4111111111111111") != reply.sensitiveValues.end(),
              "a sensitive session param is listed");
    }
    {
        auto paying = CompiledAgent::compile(parse<vasdk::flow::Agent>(replaced(kAgent, "start_page: \"start\"", "start_page: \"pay\"")),
                                             &error, nullptr, registry());
        check(paying && FlowRuntime(paying).start().expectation.secure_entry(), "a sensitive slot's question: secure entry");
    }
    // Events mask what the replies and private answers say.
    {
        auto dispatcher = std::make_shared<engine::EventDispatcher>();
        std::mutex mutex;
        std::vector<vasdk::engine::Event> events;
        dispatcher->addListener([&](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        });
        engine::EventTapProcessor tap(dispatcher);
        auto said = std::make_shared<transport::TranscriptionFrame>();
        said->transcriptionResult.text = "5551234567";
        said->isFinal = true;
        said->sensitive = true;
        tap.processFrame(said, pipeline::FlowDirection::DOWNSTREAM);
        auto reply = std::make_shared<transport::TextFrame>();
        reply->text = "We'll call 555-123-4567.";
        reply->isFinal = true;
        reply->sensitiveValues = {"5551234567", "555-123-4567"};
        tap.processFrame(reply, pipeline::FlowDirection::DOWNSTREAM);
        for (int i = 0; i < 50; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (events.size() >= 2) break;
            }
            std::this_thread::sleep_for(10ms);
        }
        std::lock_guard<std::mutex> lock(mutex);
        check(events.size() >= 2 && events[0].transcript().text() == "******4567", "a private answer's transcript is masked");
        check(events.size() >= 2 && events[1].agent_reply().text() == "We'll call ***-***-4567.", "a reply's sensitive values are masked");
    }

    // Step outcomes from other languages: only what a set step may write, all or nothing.
    {
        auto stepAgent = parse<vasdk::flow::Agent>(kAgent);
        stepAgent.mutable_flows(0)->mutable_pages(0)->mutable_routes(0)->mutable_steps(0)->mutable_custom()->set_type_url(
            "type.googleapis.com/google.protobuf.Empty");
        auto outcomes = registry();
        vasdk::flow::StepOutcome pending;
        std::string problem;
        bool applied = false;
        outcomes->addStep("google.protobuf.Empty", [&](const google::protobuf::Message&, TurnContext& turn) {
            problem.clear();
            applied = turn.apply(pending, &problem);
        });
        auto withStep = CompiledAgent::compile(stepAgent, &error, nullptr, outcomes);
        check(withStep != nullptr, "an agent with a foreign step compiles");
        if (!withStep) std::fprintf(stderr, "  %s\n", error.c_str());
        auto attempt = [&](const std::string& outcomeText) {
            pending = parse<vasdk::flow::StepOutcome>(outcomeText);
            FlowRuntime run(withStep);
            run.setToolCaller(failing);
            run.start();
            const auto reply = run.handleText("book");
            return std::make_pair(run.sessionParam("card"), reply.text);
        };
        if (withStep) {
            auto [card, text] = attempt(R"(set { key: "$session.card" value { string_value: "1" } } set { key: "$params.x" value { string_value: "2" } })");
            check(!applied && contains(problem, "$params.x") && card.kind_case() == Value::kNullValue,
                  "a $params key is refused, and nothing is applied");
            attempt(R"(set { key: "$input.text" value { string_value: "x" } })");
            check(!applied && contains(problem, "$input.text"), "$input is refused");
            attempt(R"(set { key: "$session.nobody" value { string_value: "x" } })");
            check(!applied && contains(problem, "nobody"), "an undeclared param is refused");
            attempt(R"(set { key: "$session.booking" value { string_value: "x" } })");
            check(!applied && contains(problem, "STRUCT"), "a value that doesn't fit is refused");
            attempt(R"(goto: "nowhere")");
            check(!applied && contains(problem, "nowhere"), "an unknown target is refused");
            std::tie(card, text) = attempt(R"(set { key: "$session.card" value { string_value: "4242" } } say: "Noted.")");
            check(applied && card.string_value() == "4242" && text.rfind("Noted.", 0) == 0, "a declared param is written, a text said");
        }
    }

    // filler_after_ms: the filler only when the call is slow, heard while it runs.
    {
        const auto slowAgent = CompiledAgent::compile(parse<vasdk::flow::Agent>(R"agent(
start_flow: "main"
session_params { name: "found" type: STRUCT }
intents { name: "quick" examples: ["quick"] }
intents { name: "slow" examples: ["slow"] }
tools { name: "lookup" }
flows { name: "main" start_page: "start" pages {
  name: "start"
  on_enter { ask { say { text: "Hi." } } }
  routes { intent: "quick" steps { call_tool { name: "lookup" args { key: "wait" value: "0" } into: "$session.found"
                                               filler: "One moment." filler_after_ms: 200 } }
           steps { say { text: "Found $session.found.id." } } }
  routes { intent: "slow" steps { call_tool { name: "lookup" args { key: "wait" value: "600" } into: "$session.found"
                                              filler: "One moment." filler_after_ms: 200 } }
           steps { say { text: "Found $session.found.id." } } } } }
)agent"), &error);
        check(slowAgent != nullptr, "the slow tool agent compiles");
        if (slowAgent) {
            FlowRuntime conversation(slowAgent);
            conversation.setToolCaller([](const std::string&, const nlohmann::json& args) {
                std::this_thread::sleep_for(std::chrono::milliseconds(args.value("wait", 0)));
                ToolResult result;
                result.ok = true;
                result.value = {{"id", "S-9"}};
                return result;
            });
            std::vector<std::string> heard;
            conversation.setFillerSink([&](const Reply& sofar) { heard.push_back(sofar.text); });
            conversation.start();
            auto reply = conversation.handleText("quick");
            check(heard.empty() && reply.text == "Found S-9. Hi.", "a quick call: no filler");
            reply = conversation.handleText("slow");
            check(heard == std::vector<std::string>{"One moment."} && reply.text == "Found S-9. Hi.",
                  "a slow call: the filler is heard while it runs");
        }
    }

    if (failures == 0) std::printf("security_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
