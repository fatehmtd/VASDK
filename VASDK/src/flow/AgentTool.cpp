#include "flow/AgentTool.h"

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
                 "       %s try <agent> [--entity @name] <words>\n",
                 name, name, name, name);
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
    if (argc < 3) return usage();
    spdlog::set_level(spdlog::level::warn);
    const std::string command = argv[1];
    const std::string agentPath = argv[2];

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
        for (int i = 3; i < argc; ++i) {
            const auto result = va::flow::runFlowTestFile(agent, argv[i]);
            std::printf("%s %s\n", result.passed() ? "PASS" : "FAIL", result.name.c_str());
            for (const auto& failure : result.failures) std::printf("    %s\n", failure.c_str());
            if (!result.passed()) ++failed;
        }
        std::printf("%d of %d passed\n", argc - 3 - failed, argc - 3);
        return failed == 0 ? 0 : 1;
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
        // The intent the longest example picks, among every intent (pages narrow this down).
        const auto split = va::flow::splitWords(words);
        std::string intent;
        int best = 0;
        for (const auto& candidate : agent->definition().intents()) {
            const int score = agent->matchIntent(candidate.name(), split);
            if (score > best) {
                best = score;
                intent = candidate.name();
            }
        }
        std::printf("intent: %s\n", intent.empty() ? "(none)" : (intent + " (" + std::to_string(best) + (best == 1 ? " word)" : " words)")).c_str());
        for (const auto& match : agent->entities().findAll(words)) std::printf("%s\n", describe(match).c_str());
        return 0;
    }
    if (command == "chat") {
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        va::flow::FlowRuntime conversation(agent);
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
