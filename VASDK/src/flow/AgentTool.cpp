#include "flow/AgentTool.h"
#include "engine/ProtoFileLoader.h"
#include "llm/Completion.h"
#include "nlu/LlmNluProvider.h"
#include "nlu/RuleNluProvider.h"

#include <google/protobuf/util/json_util.h>
#include <map>
#include <array>

#include "flow/CompiledAgent.h"
#include "flow/FlowRuntime.h"
#include "flow/FlowTestRunner.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace va {
namespace flow {

namespace {

int usage(const std::string& program) {
    const char* name = program.c_str();
    std::fprintf(stderr,
                 "usage: %s lint <agent>\n"
                 "       %s test <agent> <case>...\n"
                 "       %s chat <agent> [--session name=value]...\n"
                 "         (an empty line is silence, #2 taps the second choice, !<trigger> {json} fires a trigger)\n"
                 "       %s try <agent> [--entity @name] <words>\n"
                 "       %s eval <agent> <set.txtpb> [--min-accuracy 0.8]   (intent accuracy, entity F1)\n"
                 "  with an LLM after the rules (test, chat, try, eval):\n"
                 "    --llm                   the endpoint, model and key from LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY\n"
                 "                            (or OPENROUTER_API_KEY, OPENAI_API_KEY)\n"
                 "    --llm-endpoint <url>    an OpenAI-compatible chat completions URL; --llm-model, --llm-api-key\n",
                 name, name, name, name, name);
    return 2;
}

std::shared_ptr<const va::flow::CompiledAgent> load(const std::string& path, bool showWarnings,
                                                    std::shared_ptr<const FlowRegistry> registry) {
    std::string error;
    std::vector<std::string> warnings;
    auto agent = va::flow::CompiledAgent::load(path, &error, &warnings, std::move(registry));
    if (showWarnings)
        for (const auto& warning : warnings) std::fprintf(stderr, "%s: warning: %s\n", path.c_str(), warning.c_str());
    if (!agent) {
        std::string line;
        for (const char c : error + "\n") {
            if (c != '\n') { line += c; continue; }
            if (!line.empty()) std::fprintf(stderr, "%s: error: %s\n", path.c_str(), line.c_str());
            line.clear();
        }
    }
    return agent;
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Takes the --llm flags out of the arguments: an LLM to ask when given, else null.
std::shared_ptr<const llm::Completion> takeLlmFlags(std::vector<std::string>& arguments, std::string* error) {
    bool wanted = false;
    llm::LLMConfig config;
    for (size_t i = 0; i < arguments.size();) {
        const auto& flag = arguments[i];
        const bool valued = flag == "--llm-endpoint" || flag == "--llm-model" || flag == "--llm-api-key";
        if (flag != "--llm" && !valued) {
            ++i;
            continue;
        }
        if (valued && i + 1 >= arguments.size()) {
            *error = flag + " needs a value";
            return nullptr;
        }
        wanted = true;
        if (flag == "--llm-endpoint") config.endpoint = arguments[i + 1];
        if (flag == "--llm-model") config.model = arguments[i + 1];
        if (flag == "--llm-api-key") config.apiKey = arguments[i + 1];
        arguments.erase(arguments.begin() + static_cast<long>(i), arguments.begin() + static_cast<long>(i + (valued ? 2 : 1)));
    }
    if (!wanted) return nullptr;
    if (config.endpoint.empty()) config.endpoint = environment("LLM_ENDPOINT");
    if (config.model.empty()) config.model = environment("LLM_MODEL");
    for (const char* key : {"LLM_API_KEY", "OPENROUTER_API_KEY", "OPENAI_API_KEY"})
        if (config.apiKey.empty()) config.apiKey = environment(key);
    if (config.endpoint.empty()) {
        *error = "--llm: no endpoint (set LLM_ENDPOINT or pass --llm-endpoint)";
        return nullptr;
    }
    config.temperature = 0.0f;
    config.maxTokens = 400;
    config.timeoutMs = 20000;
    std::fprintf(stderr, "LLM: %s (%s)\n", config.endpoint.c_str(), config.model.empty() ? "default model" : config.model.c_str());
    return std::make_shared<llm::OpenAICompletion>(config);
}

std::string describe(const va::flow::EntityMatch& match) {
    std::string text = "@" + match.entity + " = " + va::flow::toText(match.value);
    if (match.spoken != va::flow::toText(match.value)) text += "  spoken \"" + match.spoken + "\"";
    text += "  heard \"" + match.raw + "\"";
    for (const auto& [name, value] : match.attributes.fields()) text += "  " + name + "=" + va::flow::toText(value);
    return text;
}

void print(const va::flow::Reply& reply, const va::flow::FlowRuntime& conversation) {
    std::printf("bot: %s   [%s/%s]%s%s\n", reply.text.empty() ? "(nothing)" : reply.text.c_str(),
                conversation.flowName().c_str(), conversation.pageName().c_str(),
                reply.event.empty() ? "" : ("  (" + reply.event + ")").c_str(),
                reply.endsSession ? "  (conversation ended)" : "");
    const auto stage = conversation.lookup({"nlu", "stage"});
    if (stage.has_string_value() && stage.string_value() != "rules" && !reply.intent.empty())
        std::printf("     understood by: %s (%s)\n", stage.string_value().c_str(), reply.intent.c_str());
    for (const auto& segment : reply.segments)
        if (!segment.interruptible) std::printf("     can't be interrupted: \"%s\"\n", segment.text.c_str());
    if (!reply.choices.empty()) {
        std::string labels;
        for (const auto& choice : reply.choices) labels += (labels.empty() ? "" : " | ") + choice.label;
        std::printf("     choices: %s   (#1 taps the first)\n", labels.c_str());
    }
    using Expectation = vasdk::flow::Expectation;
    const auto& expect = reply.expectation;
    if (expect.mode() != Expectation::NONE && expect.mode() != Expectation::OPEN) {
        std::string line = Expectation::Mode_Name(expect.mode());
        if (!expect.slot().empty()) line += " " + expect.slot();
        if (expect.keyboard() != Expectation::KEYBOARD_UNSET) line += ", " + Expectation::Keyboard_Name(expect.keyboard()) + " keyboard";
        std::printf("     expects: %s\n", line.c_str());
    }
}

} // namespace

int runAgentTool(int argc, char** argv, std::shared_ptr<const FlowRegistry> registry) {
    const std::string program = argc > 0 ? std::filesystem::path(argv[0]).filename().string() : "vasdk_agentc";
    auto usage = [&program] { return flow::usage(program); };
    auto load = [&registry](const std::string& path, bool showWarnings) { return flow::load(path, showWarnings, registry); };
    spdlog::set_level(spdlog::level::warn);
    std::vector<std::string> arguments(argv, argv + argc);
    std::string flagError;
    const auto completion = takeLlmFlags(arguments, &flagError);
    if (!flagError.empty()) {
        std::fprintf(stderr, "%s\n", flagError.c_str());
        return 2;
    }
    std::vector<char*> remaining;
    for (auto& argument : arguments) remaining.push_back(argument.data());
    argc = static_cast<int>(remaining.size());
    argv = remaining.data();
    if (argc < 3) return usage();
    const std::string command = argv[1];
    const std::string agentPath = argv[2];
    // Understanding: the agent's rules, then the LLM when one is given. Tests and evals wait for
    // its answer (no intent time limit), so results don't depend on how fast the network is.
    auto understander = [&completion](const std::shared_ptr<const CompiledAgent>& agent) -> std::shared_ptr<const nlu::NluProvider> {
        if (!completion) return agent->rules();
        return std::make_shared<nlu::CascadeNluProvider>(std::vector<std::shared_ptr<const nlu::NluProvider>>{
            agent->rules(), std::make_shared<nlu::LlmNluProvider>(completion, agent->definition().intents(), 0)});
    };
    auto assist = [&](const std::shared_ptr<const CompiledAgent>& agent) -> FlowTestSetup {
        if (!completion) return nullptr;
        auto nlu = understander(agent);
        return [nlu, completion](FlowRuntime& conversation) {
            conversation.setNlu(nlu);
            conversation.setLlm(completion);
        };
    };

    if (command == "lint") {
        auto agent = load(agentPath, true);
        if (!agent) return 1;
        int pages = 0;
        for (const auto& flow : agent->definition().flows()) pages += flow.pages_size();
        std::printf("%s: ok (%d flows, %d pages, %d intents, %d entities)\n", agentPath.c_str(),
                    agent->definition().flows_size(), pages, agent->definition().intents_size(),
                    agent->definition().entity_types_size());
        return 0;
    }
    if (command == "test") {
        if (argc < 4) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        int failed = 0;
        const auto setup = assist(agent);
        for (int i = 3; i < argc; ++i) {
            const auto result = va::flow::runFlowTestFile(agent, argv[i], setup);
            std::printf("%s %s\n", result.passed() ? "PASS" : "FAIL", result.name.c_str());
            for (const auto& failure : result.failures) std::printf("    %s\n", failure.c_str());
            if (!result.passed()) ++failed;
        }
        std::printf("%d of %d passed\n", argc - 3 - failed, argc - 3);
        return failed == 0 ? 0 : 1;
    }
    if (command == "eval") {   // eval <agent> <set.txtpb> [--min-accuracy 0.8]
        if (argc < 4) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        vasdk::flow::NluEvalSet set;
        std::string error;
        if (!va::engine::loadProtoFile(argv[3], set, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        double minAccuracy = 0;
        if (argc >= 6 && std::string(argv[4]) == "--min-accuracy") minAccuracy = std::atof(argv[5]);
        int right = 0;
        std::map<std::string, std::array<int, 3>> counts;   // entity → right, found, expected
        std::vector<std::string> misses;
        const auto nlu = understander(agent);
        for (const auto& item : set.cases()) {
            va::nlu::NluRequest request;
            request.text = item.text();
            request.mentions = [&](const std::string& name) { return agent->entities().find(name, item.text()).has_value(); };
            const auto understood = nlu->understand(request);
            const std::string intent = understood.intents.empty() ? std::string() : understood.intents.front().intent;
            if (intent == item.intent()) ++right;
            else misses.push_back("\"" + item.text() + "\": " + (intent.empty() ? "(none)" : intent) + ", expected " +
                                  (item.intent().empty() ? "(none)" : item.intent()));
            std::map<std::string, std::string> found;
            for (const auto& match : agent->entities().findAll(item.text())) found.emplace(match.entity, toText(match.value));
            for (const auto& [name, value] : found) {
                ++counts[name][1];
                const auto expected = item.entities().find(name);
                if (expected != item.entities().end() && expected->second == value) ++counts[name][0];
            }
            for (const auto& [name, value] : item.entities()) ++counts[name][2];
        }
        const int total = set.cases_size();
        const double accuracy = total ? static_cast<double>(right) / total : 1.0;
        std::printf("%s: %d utterances, stages %s\nintent accuracy: %.1f%% (%d of %d)\n", agentPath.c_str(), total,
                    completion ? "rules, llm" : "rules", accuracy * 100, right, total);
        for (const auto& [name, count] : counts) {
            const double precision = count[1] ? static_cast<double>(count[0]) / count[1] : 1.0;
            const double recall = count[2] ? static_cast<double>(count[0]) / count[2] : 1.0;
            const double f1 = precision + recall > 0 ? 2 * precision * recall / (precision + recall) : 0.0;
            std::printf("@%s  precision %.2f  recall %.2f  F1 %.2f\n", name.c_str(), precision, recall, f1);
        }
        for (const auto& miss : misses) std::printf("  miss %s\n", miss.c_str());
        return accuracy + 1e-9 >= minAccuracy ? 0 : 1;
    }
    if (command == "try") {
        std::string entity, words;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--entity" && i + 1 < argc) entity = va::flow::entityName(argv[++i]);
            else words += (words.empty() ? "" : " ") + argument;
        }
        if (words.empty()) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        if (!entity.empty()) {
            if (!agent->entities().knows(entity)) {
                std::fprintf(stderr, "unknown entity @%s\n", entity.c_str());
                return 1;
            }
            const auto match = agent->entities().find(entity, words);
            std::printf("%s\n", match ? describe(*match).c_str() : ("@" + entity + ": not found").c_str());
            return match ? 0 : 1;
        }
        // What the rules make of the words, among every intent (pages narrow this down).
        va::nlu::NluRequest request;
        request.text = words;
        request.mentions = [&](const std::string& name) { return agent->entities().find(name, words).has_value(); };
        const auto understood = understander(agent)->understand(request);
        if (understood.intents.empty()) std::printf("intent: (none)\n");
        else std::printf("stage: %s\n", understood.stage.c_str());
        for (size_t i = 0; i < understood.intents.size() && i < 3; ++i) {
            const auto& match = understood.intents[i];
            std::printf("%s %s (score %.2f, confidence %.2f)\n", i == 0 ? "intent:" : "   or:", match.intent.c_str(), match.score,
                        match.confidence);
        }
        for (const auto& match : agent->entities().findAll(words)) std::printf("%s\n", describe(match).c_str());
        return 0;
    }
    if (command == "chat") {
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        va::flow::FlowRuntime conversation(agent);
        if (const auto setup = assist(agent)) setup(conversation);
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument != "--session" || i + 1 >= argc) return usage();
            const std::string assignment = argv[++i];
            const auto split = assignment.find('=');
            if (split == std::string::npos) return usage();
            // A number, true/false, or else a string; the param's type converts it.
            const std::string text = assignment.substr(split + 1);
            char* end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            const auto value = text == "true" || text == "false" ? va::flow::boolValue(text == "true")
                             : (!text.empty() && *end == '\0') ? va::flow::numberValue(number)
                             : va::flow::stringValue(text);
            std::string error;
            if (!conversation.setSessionParam(assignment.substr(0, split), value, &error)) {
                std::fprintf(stderr, "--session %s: %s\n", assignment.c_str(), error.c_str());
                return 1;
            }
        }
        print(conversation.start(), conversation);
        std::string line;
        while (std::printf("you: "), std::fflush(stdout), std::getline(std::cin, line)) {
            const bool tap = line.size() > 1 && line[0] == '#' && std::isdigit(static_cast<unsigned char>(line[1]));
            if (line.size() > 1 && line[0] == '!') {   // "!car-ready {"car": "Golf"}": the app fires a trigger
                const auto space = line.find(' ');
                google::protobuf::Struct payload;
                if (space != std::string::npos &&
                    !google::protobuf::util::JsonStringToMessage(line.substr(space + 1), &payload).ok()) {
                    std::printf("(the payload must be a JSON object)\n");
                    continue;
                }
                const auto reply = conversation.fireTrigger(line.substr(1, space == std::string::npos ? std::string::npos : space - 1),
                                                            payload);
                for (const auto& trigger : reply.triggers)
                    std::printf("(trigger %s: %s%s%s)\n", trigger.name.c_str(),
                                vasdk::flow::TriggerDecision_Name(trigger.decision).c_str(), trigger.reason.empty() ? "" : ", ",
                                trigger.reason.c_str());
                if (!reply.text.empty()) print(reply, conversation);
                continue;
            }
            print(line.empty() ? conversation.handleNoInput()
                  : tap        ? conversation.selectChoice(std::atoi(line.c_str() + 1) - 1)
                               : conversation.handleText(line),
                  conversation);
        }
        std::printf("\n");
        return 0;
    }
    return usage();
}

} // namespace flow
} // namespace va
