// include_files: an agent in several files, each a part of it (an app's verticals), merged in.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "vasdk_agent_include";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "climate");
    std::filesystem::create_directories(dir / "media" / "goals");

    std::ofstream(dir / "agent.txtpb")
        << "name: \"car\" start_flow: \"drive\"\n"
        << "include_files: [\"climate/climate.txtpb\", \"media/media.txtpb\"]\n"
        << "envelopes: [\"standard\"]\n"
        << "intents { name: \"help\" examples: [\"help\"] }\n"
        << "envelope_defs { name: \"standard\" routes { intent: \"help\" steps { say { text: \"Ask for the climate or the music.\" } } } }\n"
        << "flows { name: \"drive\" start_page: \"ready\" pages { name: \"ready\" on_enter { say { text: \"Hi.\" } } } }\n";
    // A vertical: its intents, an entity, an envelope applied to the whole agent, a trigger
    std::ofstream(dir / "climate" / "climate.txtpb")
        << "envelopes: [\"climate\"]\n"
        << "intents { name: \"warmer\" examples: [\"warmer\", \"i'm cold\"] }\n"
        << "entity_types { name: \"zone\" list { values { value: \"driver\" } values { value: \"passenger\" } } }\n"
        << "envelope_defs { name: \"climate\" routes { intent: \"warmer\" steps { say { text: \"Warmer.\" } } } }\n"
        << "triggers { name: \"hot\" source { app_event { name: \"hot\" } } action { say { text: \"It's hot.\" } } }\n";
    // Another, with its own flow and a goal file relative to itself
    std::ofstream(dir / "media" / "media.txtpb")
        << "envelopes: [\"media\"]\n"
        << "goal_files: [\"goals/playlist.txtpb\"]\n"
        << "intents { name: \"playlist\" examples: [\"make a playlist\"] }\n"
        << "envelope_defs { name: \"media\" routes { intent: \"playlist\" target: \"flow:playlist\" } }\n"
        << "flows { name: \"playlist\" start_page: \"name\" pages { name: \"name\" form { goal: \"playlist\" }\n"
        << "  routes { condition: \"$form.complete\" steps { say { text: \"Made $form.title.\" } } target: \"END_FLOW\" } } }\n";
    std::ofstream(dir / "media" / "goals" / "playlist.txtpb")
        << "name: \"playlist\" slots { name: \"title\" type: STRING prompt: \"What should it be called?\" }\n";

    std::string error;
    std::vector<std::string> warnings;
    auto agent = va::flow::CompiledAgent::load((dir / "agent.txtpb").string(), &error, &warnings);
    check(agent != nullptr, "an agent in three files loads: " + error);
    if (agent) {
        const auto& definition = agent->definition();
        check(definition.intents_size() == 3 && definition.flows_size() == 2 && definition.goals_size() == 1 &&
                  definition.triggers_size() == 1 && definition.entity_types_size() == 1,
              "every part is merged in");
        check(definition.envelopes_size() == 3 && definition.envelopes(0) == "standard" && definition.envelopes(1) == "climate",
              "the main file's envelopes first, then each part's, in order");
        check(definition.include_files_size() == 0 && definition.goal_files_size() == 0, "the files are resolved");
        va::flow::FlowRuntime drive(agent);
        check(drive.start().text == "Hi.", "the main file's start flow");
        check(drive.handleText("I'm cold").text == "Warmer.", "a part's envelope answers on the main file's page");
        check(drive.handleText("help").text == "Ask for the climate or the music.", "the main file's own envelope too");
        check(drive.handleText("make a playlist").text == "What should it be called?", "a part's flow, with its goal file");
        check(drive.handleText("road trip").text == "Made road trip.", "and its form");
        check(drive.fireTrigger("hot").text == "It's hot.", "a part's trigger");
    }

    // What a part may not do
    std::ofstream(dir / "owner.txtpb") << "start_flow: \"drive\" include_files: [\"bad.txtpb\"]\n";
    std::ofstream(dir / "bad.txtpb") << "name: \"other\" start_flow: \"x\"\n";
    check(!va::flow::CompiledAgent::load((dir / "owner.txtpb").string(), &error) &&
              contains(error, "bad.txtpb") && contains(error, "name, start_flow: only the main agent file sets these"),
          "a part can't rename the agent or move its start: " + error);
    std::ofstream(dir / "loop.txtpb") << "start_flow: \"drive\" include_files: [\"loop_part.txtpb\"]\n";
    std::ofstream(dir / "loop_part.txtpb") << "include_files: [\"loop_part.txtpb\"]\n";
    check(!va::flow::CompiledAgent::load((dir / "loop.txtpb").string(), &error) && contains(error, "includes itself"),
          "a part that includes itself: " + error);
    std::ofstream(dir / "missing.txtpb") << "start_flow: \"drive\" include_files: [\"nowhere.txtpb\"]\n";
    check(!va::flow::CompiledAgent::load((dir / "missing.txtpb").string(), &error) && contains(error, "nowhere.txtpb"),
          "a missing part, named: " + error);
    std::ofstream(dir / "twice.txtpb") << "start_flow: \"drive\" include_files: [\"climate/climate.txtpb\", \"climate/climate.txtpb\"]\n"
                                       << "flows { name: \"drive\" start_page: \"ready\" pages { name: \"ready\" on_enter { say { text: \"Hi.\" } } } }\n";
    check(!va::flow::CompiledAgent::load((dir / "twice.txtpb").string(), &error) && contains(error, "warmer"),
          "the same part twice: its names are duplicates: " + error);

    std::filesystem::remove_all(dir);
    if (failures == 0) std::printf("agent_include_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
