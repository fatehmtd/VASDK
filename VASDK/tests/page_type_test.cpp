// Page types: the built-in menu, yes_no and end; one written in text proto, its params typed by
// a .proto loaded at runtime (proto_imports, from text and JSON agents) and read as $params; one
// written in C++ with typed params. Load errors: unknown types, wrong or missing params, $params
// fields that don't exist, menu targets that don't exist, a missing import.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/wrappers.pb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace va;
using namespace va::flow;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

const char* kInfoProto = R"proto(
syntax = "proto3";
package acme;
message InfoParams {
  string text = 1;
  string more = 2;
}
)proto";

const char* kAgent = R"agent(
start_flow: "main"
proto_imports: ["acme.proto"]
intents { name: "more" examples: ["more"] }
intents { name: "help" examples: ["help"] }
page_types {
  name: "info"
  params_type: "acme.InfoParams"
  on_enter { ask { say { text: "$params.text Want more?" } } }
  routes { intent: "more" steps { say { text: "$params.more" } } target: "done" }
}
page_types { name: "end" on_enter { say { text: "My own end." } } on_enter { end {} } }
flows {
  name: "main"
  start_page: "menu"
  pages {
    name: "menu"
    type: "menu"
    params { [type.googleapis.com/vasdk.flow.MenuParams] {
      prompt: "Hours, prices, or a person?"
      options { label: "Hours" target: "hours" }
      options { label: "Prices" value: "prices" synonyms: ["how much"] target: "prices" }
      options { label: "A person" target: "check" }
      options { label: "Other" }
    } }
    routes { intent: "help" steps { say { text: "Pick one." } } }
  }
  pages { name: "hours" type: "info" params { [type.googleapis.com/acme.InfoParams] { text: "Open 8 to 6." more: "Saturdays too." } } }
  pages { name: "prices" type: "info" params { [type.googleapis.com/acme.InfoParams] { text: "Oil is 49." more: "Tires are 89." } } }
  pages {
    name: "check"
    type: "yes_no"
    params { [type.googleapis.com/vasdk.flow.YesNoParams] { question: "Put you through?" yes_target: "bye" no_target: "menu" } }
  }
  pages { name: "bye" type: "end" }
  pages { name: "done" type: "counter" params { [type.googleapis.com/google.protobuf.Int32Value] { value: 3 } } }
}
)agent";

// A page type in C++: asks for a number up to its params' value; others go to the routes.
std::shared_ptr<FlowRegistry> registry() {
    auto registry = std::make_shared<FlowRegistry>();
    registry->addPageType<google::protobuf::Int32Value>(
        "counter",
        [](const google::protobuf::Int32Value& most, TurnContext& turn) {
            turn.ask("Pick a number up to " + std::to_string(most.value()) + ".");
        },
        [](const google::protobuf::Int32Value& most, TurnContext& turn) {
            const int number = std::atoi(turn.text().c_str());
            if (number < 1 || number > most.value()) return false;
            turn.say("You picked " + turn.text() + ".");
            turn.goTo(kTargetEndSession);
            return true;
        });
    return registry;
}

std::string loadError(const fs::path& directory, const std::string& replace, const std::string& with,
                      std::shared_ptr<const FlowRegistry> extensions) {
    std::string text = kAgent;
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    text.replace(at, replace.size(), with);
    write(directory / "broken.txtpb", text);
    std::string error;
    return CompiledAgent::load((directory / "broken.txtpb").string(), &error, nullptr, std::move(extensions)) ? "" : error;
}

} // namespace

int main() {
    const fs::path directory =
        fs::temp_directory_path() / ("vasdk_page_type_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    write(directory / "acme.proto", kInfoProto);
    write(directory / "agent.txtpb", kAgent);

    std::string error;
    std::vector<std::string> warnings;
    auto agent = CompiledAgent::load((directory / "agent.txtpb").string(), &error, &warnings, registry());
    check(agent != nullptr, "the agent loads, with acme.proto imported at runtime");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        fs::remove_all(directory);
        return 1;
    }
    bool unreachedWarning = false;
    for (const auto& warning : warnings) unreachedWarning = unreachedWarning || contains(warning, "never reached");
    check(!unreachedWarning, "pages a menu or yes_no leads to count as reached");

    // menu: its options as choices; naming one goes to its target; anything else to the routes.
    FlowRuntime conversation(agent);
    auto reply = conversation.start();
    check(reply.text == "Hours, prices, or a person?" && reply.choices.size() == 4 && reply.choices[1].value == "prices",
          "menu: the prompt, its options as choices");
    reply = conversation.handleText("help");
    check(reply.text == "Pick one. Hours, prices, or a person?", "menu: other words go to the page's routes");
    reply = conversation.handleText("other");
    check(!reply.matched && conversation.pageName() == "menu", "menu: an option without a target goes to the routes");

    // A page type in text proto: its steps and routes, its params as $params.
    reply = conversation.handleText("um, how much is it");
    check(conversation.pageName() == "prices" && reply.text == "Oil is 49. Want more?",
          "menu: a synonym within the words; the info type's on_enter reads $params");
    reply = conversation.handleText("more please");
    check(contains(reply.text, "Tires are 89.") && conversation.pageName() == "done",
          "the type's routes, with the page's own params");

    // A page type in C++, typed params.
    check(contains(reply.text, "Pick a number up to 3."), "a C++ page type's onEnter, typed params");
    reply = conversation.handleText("7");
    check(!reply.matched && conversation.pageName() == "done", "onTurn false: the routes get it (none here)");
    reply = conversation.handleText("2");
    check(reply.text == "You picked 2." && reply.endsSession, "onTurn true: answered");

    // yes_no and a declared type replacing a built-in one.
    conversation.start();
    reply = conversation.selectChoice(0);
    check(conversation.pageName() == "hours" && reply.text == "Open 8 to 6. Want more?", "menu: a tapped choice");
    conversation.start();
    reply = conversation.handleText("a person");
    check(conversation.pageName() == "check" && reply.text == "Put you through?" &&
              reply.expectation.mode() == vasdk::flow::Expectation::YES_NO,
          "yes_no: asks, expecting yes or no");
    reply = conversation.handleText("maybe");
    check(!reply.matched && conversation.pageName() == "check", "yes_no: neither goes to the routes");
    reply = conversation.handleText("no thanks");
    check(conversation.pageName() == "menu", "yes_no: no goes to no_target");
    conversation.handleText("a person");
    reply = conversation.handleText("yes");
    check(reply.text == "My own end." && reply.endsSession, "a declared page type replaces the built-in of its name");

    // The same kind of agent in JSON, params from proto_imports.
    write(directory / "agent.json", R"json({
      "startFlow": "main",
      "protoImports": ["acme.proto"],
      "pageTypes": [{"name": "info", "paramsType": "acme.InfoParams", "onEnter": [{"say": {"text": ["$params.text"]}}]}],
      "flows": [{"name": "main", "startPage": "hours", "pages": [
        {"name": "hours", "type": "info",
         "params": {"@type": "type.googleapis.com/acme.InfoParams", "text": "Open 8 to 6."},
         "onEnter": [{"end": {}}]}]}]
    })json");
    auto jsonAgent = CompiledAgent::load((directory / "agent.json").string(), &error);
    check(jsonAgent && FlowRuntime(jsonAgent).start().text == "Open 8 to 6.", "a JSON agent with proto_imports");
    if (!jsonAgent) std::fprintf(stderr, "  %s\n", error.c_str());

    // Load errors
    auto extensions = registry();
    check(contains(loadError(directory, "type: \"counter\"", "type: \"countr\"", extensions), "unknown page type \"countr\""),
          "an unknown page type");
    check(contains(loadError(directory, "type: \"counter\"", "type: \"counter\"", nullptr), "unknown page type \"counter\""),
          "a C++ page type needs its registry");
    check(contains(loadError(directory, "[type.googleapis.com/google.protobuf.Int32Value] { value: 3 }",
                             "[type.googleapis.com/acme.InfoParams] { text: \"x\" }", extensions),
                   "params are \"acme.InfoParams\", but page type \"counter\" takes \"google.protobuf.Int32Value\""),
          "params of the wrong type");
    check(contains(loadError(directory, "pages { name: \"bye\" type: \"end\" }", "pages { name: \"bye\" type: \"info\" }", extensions),
                   "needs params of type \"acme.InfoParams\""),
          "missing params");
    check(contains(loadError(directory, "$params.more", "$params.most", extensions), "$params.most"), "$params fields are checked");
    check(contains(loadError(directory, "target: \"prices\"", "target: \"pricez\"", extensions), "unknown target \"pricez\""),
          "a menu's targets are checked");
    check(contains(loadError(directory, "proto_imports: [\"acme.proto\"]", "proto_imports: [\"acme2.proto\"]", extensions),
                   "proto_imports \"acme2.proto\""),
          "a missing import");

    fs::remove_all(directory);
    if (failures == 0) std::printf("page_type_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
