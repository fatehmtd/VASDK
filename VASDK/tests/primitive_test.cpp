// Steps and envelopes written in C++: a step with typed params says, stores and reads values; one
// moves the conversation; one registered by type name gets params from a .proto loaded at
// runtime. Envelopes run before the routes outermost first (one rewrites the user's words, one
// answers "thanks" itself) and after the reply innermost first (one counts turns). Load errors:
// a step nobody registered, an unknown envelope, params of an unknown type.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>
#include <google/protobuf/wrappers.pb.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace va;
using namespace va::flow;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kAgent = R"agent(
start_flow: "main"
session_params { name: "turns" type: NUMBER default { number_value: 0 } }
session_params { name: "name" type: STRING default { string_value: "Sam" } }
intents { name: "card" examples: ["card"] }
intents { name: "go" examples: ["go"] }
envelopes: ["outer", "counting"]
flows {
  name: "main"
  start_page: "welcome"
  envelopes: ["thanks"]
  pages {
    name: "welcome"
    envelopes: ["inner", "redact"]
    on_enter { custom { [type.googleapis.com/google.protobuf.StringValue] { value: "Hi $session.name." } } }
    on_enter { ask { say { text: "Card or go?" } } }
    routes { intent: "card" steps { say { text: "Got $input.text." } } }
    routes { intent: "go" steps { custom { [type.googleapis.com/google.protobuf.Int32Value] { value: 2 } } }
                          steps { say { text: "Not reached." } } }
  }
  pages { name: "two" on_enter { say { text: "Page two." } } on_enter { end {} } }
}
)agent";

std::vector<std::string> order;   // which envelopes ran, in order

std::shared_ptr<FlowRegistry> registry() {
    auto registry = std::make_shared<FlowRegistry>();
    registry->addStep<google::protobuf::StringValue>([](const google::protobuf::StringValue& greeting, TurnContext& turn) {
        turn.say(turn.render(greeting.value()));
        turn.set("$page.locals.greeted", boolValue(true));
    });
    registry->addStep<google::protobuf::Int32Value>([](const google::protobuf::Int32Value& page, TurnContext& turn) {
        if (page.value() == 2) turn.goTo("two");
    });
    auto logged = [](std::string name) {
        FlowRegistry::Envelope envelope;
        envelope.before = [name](TurnContext&) { order.push_back("before " + name); return false; };
        envelope.after = [name](TurnContext&) { order.push_back("after " + name); };
        return envelope;
    };
    registry->addEnvelope("outer", logged("outer"));
    registry->addEnvelope("inner", logged("inner"));
    FlowRegistry::Envelope counting;
    counting.after = [](TurnContext& turn) {
        turn.set("$session.turns", numberValue(turn.lookup("$session.turns").number_value() + 1));
    };
    registry->addEnvelope("counting", counting);
    FlowRegistry::Envelope thanks;
    thanks.before = [](TurnContext& turn) {
        if (splitWords(turn.text()) != std::vector<std::string>{"thanks"}) return false;
        turn.say("You're welcome.");
        return true;
    };
    registry->addEnvelope("thanks", thanks);
    FlowRegistry::Envelope redact;
    redact.before = [](TurnContext& turn) {
        std::string text = turn.text();
        std::replace_if(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }, '#');
        turn.setText(text);
        return false;
    };
    registry->addEnvelope("redact", redact);
    return registry;
}

std::shared_ptr<const CompiledAgent> compileText(const std::string& text, std::string* error,
                                                 std::shared_ptr<const FlowRegistry> extensions) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error, nullptr, std::move(extensions));
}

std::string compileError(const std::string& replace, const std::string& with, std::shared_ptr<const FlowRegistry> extensions) {
    std::string text = kAgent;
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    text.replace(at, replace.size(), with);
    std::string error;
    return compileText(text, &error, std::move(extensions)) ? "" : error;
}

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error, registry());
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }

    // A step with typed params: says, stores; envelopes after the reply, innermost first.
    FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.text == "Hi Sam. Card or go?", "a C++ step says, with the template rendered");
    check(conversation.lookup({"page", "locals", "greeted"}).bool_value(), "a C++ step stores a value");
    check(order == std::vector<std::string>{"after inner", "after outer"}, "after: innermost first");
    check(conversation.sessionParam("turns").number_value() == 1, "an envelope's after counts the greeting");

    // Before the routes, outermost first; one rewrites the words the routes see.
    order.clear();
    reply = conversation.handleText("my card is 4111 1111");
    check(reply.text == "Got my card is #### ####. Card or go?", "an envelope's before rewrites the words");
    check(order == std::vector<std::string>{"before outer", "before inner", "after inner", "after outer"},
          "before: outermost first");
    check(conversation.sessionParam("turns").number_value() == 2, "counted");

    // An envelope answers the turn itself: the question is asked again.
    reply = conversation.handleText("Thanks!");
    check(reply.matched && reply.text == "You're welcome. Card or go?", "an envelope answers; the question again");

    // A step moves the conversation: the steps after it don't run.
    reply = conversation.handleText("go");
    check(conversation.pageName() == "two" && reply.text == "Page two." && reply.endsSession, "a C++ step's goTo");

    // A step registered by its type's name, its params from a .proto loaded at runtime.
    {
        const fs::path directory =
            fs::temp_directory_path() / ("vasdk_primitive_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        std::ofstream(directory / "acme.proto") << "syntax = \"proto3\";\npackage acme;\nmessage Points { string into = 1; int32 bonus = 2; }\n";
        std::ofstream(directory / "agent.txtpb") << R"agent(
start_flow: "main"
proto_imports: ["acme.proto"]
session_params { name: "points" type: NUMBER }
flows { name: "main" start_page: "start" pages {
  name: "start"
  on_enter { custom { [type.googleapis.com/acme.Points] { into: "$session.points" bonus: 5 } } }
  on_enter { say { text: "You have $session.points points." } }
  on_enter { end {} } } }
)agent";
        auto extensions = std::make_shared<FlowRegistry>();
        extensions->addStep("acme.Points", [](const google::protobuf::Message& params, TurnContext& turn) {
            const auto* type = params.GetDescriptor();
            const auto* reflection = params.GetReflection();
            turn.set(reflection->GetString(params, type->FindFieldByName("into")),
                     numberValue(10.0 * reflection->GetInt32(params, type->FindFieldByName("bonus"))));
        });
        auto imported = CompiledAgent::load((directory / "agent.txtpb").string(), &error, nullptr, extensions);
        check(imported && FlowRuntime(imported).start().text == "You have 50 points.", "a step by type name, params imported");
        if (!imported) std::fprintf(stderr, "  %s\n", error.c_str());
        fs::remove_all(directory);
    }

    // Load errors
    check(contains(compileError("", "", nullptr), "no step is registered for \"google.protobuf.StringValue\""),
          "a step nobody registered");
    check(contains(compileError("envelopes: [\"thanks\"]", "envelopes: [\"thank\"]", registry()), "unknown envelope \"thank\""),
          "an unknown envelope");
    {
        vasdk::flow::Agent definition;
        google::protobuf::TextFormat::ParseFromString(kAgent, &definition);
        auto* custom = definition.mutable_flows(0)->mutable_pages(1)->add_on_enter()->mutable_custom();
        custom->set_type_url("type.googleapis.com/acme.Missing");
        std::string missing;
        check(!CompiledAgent::compile(definition, &missing, nullptr, registry()) &&
                  contains(missing, "params type \"acme.Missing\" is unknown"),
              "params of a type that isn't linked in or imported");
    }

    if (failures == 0) std::printf("primitive_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
