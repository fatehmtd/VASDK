// Phrase sets in an agent of several files: never one merged namespace. Each file (the main one,
// each include_files part) expands its own examples with its own sets and the packs it uses, so
// two parts may each have an "up" of their own; a part can't see another's; a pack two parts use
// is read once; ${pack.set} names a pack. And what lint says: unused sets and packs, one set
// hiding another, and one sentence in two intents matched on the same page.

#include <VASDK/flow/CompiledAgent.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace va::flow;

namespace {

class PhraseSetScopesTest : public testing::Test {
protected:
    void write(const std::string& name, const std::string& text) {
        const auto path = _folder / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }
    std::shared_ptr<const CompiledAgent> load(const std::string& name = "agent.txtpb") {
        _error.clear();
        _warnings.clear();
        return CompiledAgent::load((_folder / name).string(), &_error, &_warnings);
    }
    std::vector<std::string> examples(const CompiledAgent& agent, const std::string& intent) const {
        for (const auto& declared : agent.definition().intents())
            if (declared.name() == intent) return {declared.examples().begin(), declared.examples().end()};
        return {};
    }
    bool warned(const std::string& part) const {
        return std::any_of(_warnings.begin(), _warnings.end(), [&](const std::string& w) { return w.find(part) != std::string::npos; });
    }
    std::string allWarnings() const {
        std::string all;
        for (const auto& warning : _warnings) all += "\n  " + warning;
        return all;
    }

    va::test::UniqueTemporaryDirectory _folder{"vasdk_phrase_sets"};
    std::string _error;
    std::vector<std::string> _warnings;
};

// A main file whose page routes every intent named, so lint's "not used by any route" stays quiet.
std::string mainFile(const std::vector<std::string>& intents, const std::string& more = {}) {
    std::string routes;
    for (const auto& intent : intents) routes += "routes { intent: \"" + intent + "\" steps { say { text: \"ok\" } } }\n";
    return "name: \"scopes\"\nstart_flow: \"main\"\n" + more +
           "\nflows { name: \"main\" start_page: \"idle\" pages { name: \"idle\" " + routes + " } }\n";
}

TEST_F(PhraseSetScopesTest, EachPartExpandsWithItsOwnSets) {
    write("agent.txtpb", mainFile({"louder", "warmer"}, "include_files: [\"media.txtpb\", \"climate.txtpb\"]"));
    write("media.txtpb", "phrase_sets { name: \"up\" phrases: [\"turn [it] up\", \"louder\"] }\n"
                         "intents { name: \"louder\" examples: [\"${up}\"] }\n");
    write("climate.txtpb", "phrase_sets { name: \"up\" phrases: [\"warmer\", \"turn up the heat\"] }\n"
                           "intents { name: \"warmer\" examples: [\"${up}\"] }\n");
    const auto agent = load();
    ASSERT_NE(agent, nullptr) << _error;
    EXPECT_EQ(examples(*agent, "louder"), (std::vector<std::string>{"turn it up", "turn up", "louder"}));
    EXPECT_EQ(examples(*agent, "warmer"), (std::vector<std::string>{"warmer", "turn up the heat"}))
        << "the same name, its own phrases: no collision";
    EXPECT_TRUE(agent->definition().phrase_sets().empty()) << "no set survives into the merged agent";
}

TEST_F(PhraseSetScopesTest, APartDoesntSeeAnothersSets) {
    write("agent.txtpb", mainFile({"louder", "quieter"}, "include_files: [\"media.txtpb\", \"other.txtpb\"]"));
    write("media.txtpb", "phrase_sets { name: \"up\" phrases: [\"louder\"] }\nintents { name: \"louder\" examples: [\"${up}\"] }\n");
    write("other.txtpb", "intents { name: \"quieter\" examples: [\"not ${up}\"] }\n");
    EXPECT_EQ(load(), nullptr);
    EXPECT_NE(_error.find("other.txtpb: intent \"quieter\" example \"not ${up}\""), std::string::npos) << _error;
    EXPECT_NE(_error.find("no phrase set \"up\""), std::string::npos) << _error;
}

TEST_F(PhraseSetScopesTest, NoPartSeesTheMainFilesSets) {
    write("agent.txtpb", mainFile({"louder"}, "include_files: [\"media.txtpb\"]\nphrase_sets { name: \"up\" phrases: [\"louder\"] }"));
    write("media.txtpb", "intents { name: \"louder\" examples: [\"${up}\"] }\n");
    EXPECT_EQ(load(), nullptr);
    EXPECT_NE(_error.find("no phrase set \"up\""), std::string::npos) << _error;
}

TEST_F(PhraseSetScopesTest, APackTwoPartsUseIsSharedAndNamed) {
    write("phrases/devices.txtpb", "name: \"devices\"\n"
                                  "phrase_sets { name: \"on\" phrases: [\"(turn|switch) on\"] }\n"
                                  "phrase_sets { name: \"off\" phrases: [\"(turn|switch) off\"] }\n");
    write("agent.txtpb", mainFile({"light_on", "radio_off"}, "include_files: [\"parts/light.txtpb\", \"parts/radio.txtpb\"]"));
    write("parts/light.txtpb", "phrase_set_files: [\"../phrases/devices.txtpb\"]\n"
                               "intents { name: \"light_on\" examples: [\"${devices.on} the light\"] }\n");
    write("parts/radio.txtpb", "phrase_set_files: [\"../phrases/devices.txtpb\"]\n"
                               "intents { name: \"radio_off\" examples: [\"${off} the radio\"] }\n");
    const auto agent = load();
    ASSERT_NE(agent, nullptr) << _error;
    EXPECT_EQ(examples(*agent, "light_on"), (std::vector<std::string>{"turn on the light", "switch on the light"}));
    EXPECT_EQ(examples(*agent, "radio_off"), (std::vector<std::string>{"turn off the radio", "switch off the radio"}));
    EXPECT_FALSE(warned("uses pack \"devices\"")) << "each part used a set of it" << allWarnings();
}

TEST_F(PhraseSetScopesTest, NearerSetsHideFartherOnesWithANote) {
    write("devices.txtpb", "name: \"devices\"\nphrase_sets { name: \"on\" phrases: [\"power on\"] }\n");
    write("agent.txtpb", mainFile({"light_on", "radio_on"},
                                  "phrase_set_files: [\"devices.txtpb\"]\n"
                                  "phrase_sets { name: \"on\" phrases: [\"turn on\"] }\n"
                                  "intents { name: \"light_on\" phrase_sets { name: \"on\" phrases: [\"light up\"] } examples: [\"${on}\"] }\n"
                                  "intents { name: \"radio_on\" examples: [\"${on} the radio\", \"${devices.on} the radio\"] }"));
    const auto agent = load();
    ASSERT_NE(agent, nullptr) << _error;
    EXPECT_EQ(examples(*agent, "light_on"), std::vector<std::string>{"light up"});
    EXPECT_EQ(examples(*agent, "radio_on"), (std::vector<std::string>{"turn on the radio", "power on the radio"}));
    EXPECT_TRUE(warned("note: intent \"light_on\": its phrase set \"on\" hides the file's")) << allWarnings();
    EXPECT_TRUE(warned("note: agent.txtpb: its phrase set \"on\" hides pack \"devices\"'s")) << allWarnings();
}

TEST_F(PhraseSetScopesTest, UnusedSetsAndPacksAreWarnedAbout) {
    write("devices.txtpb", "name: \"devices\"\nphrase_sets { name: \"on\" phrases: [\"power on\"] }\n");
    write("agent.txtpb", mainFile({"hello"}, "phrase_set_files: [\"devices.txtpb\"]\n"
                                             "phrase_sets { name: \"spare\" phrases: [\"x\"] }\n"
                                             "intents { name: \"hello\" examples: [\"hello\"] }"));
    ASSERT_NE(load(), nullptr) << _error;
    EXPECT_TRUE(warned("agent.txtpb: phrase set \"spare\" is never used")) << allWarnings();
    EXPECT_TRUE(warned("agent.txtpb: uses pack \"devices\" (devices.txtpb), but none of its sets")) << allWarnings();
}

TEST_F(PhraseSetScopesTest, PackMistakesAreLoadErrors) {
    write("agent.txtpb", mainFile({"hello"}, "phrase_set_files: [\"missing.txtpb\"]\nintents { name: \"hello\" examples: [\"hello\"] }"));
    EXPECT_EQ(load(), nullptr);
    EXPECT_NE(_error.find("phrase_set_files \"missing.txtpb\""), std::string::npos) << _error;

    write("nameless.txtpb", "phrase_sets { name: \"on\" phrases: [\"power on\"] }\n");
    write("agent.txtpb", mainFile({"hello"}, "phrase_set_files: [\"nameless.txtpb\"]\nintents { name: \"hello\" examples: [\"hello\"] }"));
    EXPECT_EQ(load(), nullptr);
    EXPECT_NE(_error.find("the pack needs a name"), std::string::npos) << _error;

    write("agent.txtpb", mainFile({"hello"}, "phrase_sets { name: \"on\" phrases: [\"a\"] }\nphrase_sets { name: \"on\" phrases: [\"b\"] }\n"
                                             "intents { name: \"hello\" examples: [\"${on}\"] }"));
    EXPECT_EQ(load(), nullptr);
    EXPECT_NE(_error.find("phrase set \"on\" is declared twice"), std::string::npos) << _error;
}

TEST_F(PhraseSetScopesTest, ASentenceInTwoIntentsOnOnePageIsWarnedAbout) {
    write("agent.txtpb", mainFile({"louder", "warmer"}, "include_files: [\"media.txtpb\", \"climate.txtpb\"]"));
    write("media.txtpb", "intents { name: \"louder\" examples: [\"turn [it] up\"] }\n");
    write("climate.txtpb", "intents { name: \"warmer\" examples: [\"turn (it|the heat) up\"] }\n");
    ASSERT_NE(load(), nullptr) << _error;
    EXPECT_TRUE(warned("intents \"louder\" (media.txtpb) and \"warmer\" (climate.txtpb): both have \"turn it up\"")) << allWarnings();
}

TEST_F(PhraseSetScopesTest, ASentenceInTwoIntentsOnDifferentPagesIsFine) {
    write("agent.txtpb",
          "name: \"pages\"\nstart_flow: \"main\"\n"
          "intents { name: \"louder\" examples: [\"turn it up\"] }\nintents { name: \"warmer\" examples: [\"turn it up\"] }\n"
          "flows { name: \"main\" start_page: \"media\"\n"
          "  pages { name: \"media\" routes { intent: \"louder\" target: \"climate\" } }\n"
          "  pages { name: \"climate\" routes { intent: \"warmer\" target: \"media\" } } }\n");
    ASSERT_NE(load(), nullptr) << _error;
    EXPECT_FALSE(warned("both have")) << allWarnings();
}

TEST_F(PhraseSetScopesTest, APageRouteOverridingAnEnvelopesIsntACollision) {
    write("agent.txtpb",
          "name: \"override\"\nstart_flow: \"main\"\nenvelopes: [\"standard\"]\n"
          "intents { name: \"help\" examples: [\"help\", \"options\"] }\nintents { name: \"route_options\" examples: [\"options\"] }\n"
          "intents { name: \"menu\" examples: [\"options\"] }\n"
          "envelope_defs { name: \"standard\" routes { intent: \"help\" steps { say { text: \"Help.\" } } }\n"
          "                                   routes { intent: \"menu\" steps { say { text: \"Menu.\" } } } }\n"
          "flows { name: \"main\" start_page: \"routes\"\n"
          "  pages { name: \"routes\" routes { intent: \"route_options\" steps { say { text: \"Others.\" } } } } }\n");
    ASSERT_NE(load(), nullptr) << _error;
    EXPECT_FALSE(warned("\"route_options\"")) << "the page's own route wins over the envelope's" << allWarnings();
    EXPECT_TRUE(warned("intents \"help\" (agent.txtpb) and \"menu\" (agent.txtpb): both have \"options\""))
        << "two routes of the same envelope compete" << allWarnings();
}

TEST_F(PhraseSetScopesTest, KnowledgeQuestionsAndNegativeExamplesExpandToo) {
    write("agent.txtpb", mainFile({"cancel"}, "phrase_sets { name: \"stop\" phrases: [\"cancel\", \"stop\"] }\n"
                                              "intents { name: \"cancel\" examples: [\"${stop}\"] negative_examples: [\"(don't|do not) ${stop}\"] }\n"
                                              "knowledge { questions: [\"do you (do|fix) dents\"] answer: \"No.\" }"));
    const auto agent = load();
    ASSERT_NE(agent, nullptr) << _error;
    const auto& intent = agent->definition().intents(0);
    EXPECT_EQ(std::vector<std::string>(intent.negative_examples().begin(), intent.negative_examples().end()),
              (std::vector<std::string>{"don't cancel", "don't stop", "do not cancel", "do not stop"}));
    const auto& questions = agent->definition().knowledge(0).questions();
    EXPECT_EQ(std::vector<std::string>(questions.begin(), questions.end()),
              (std::vector<std::string>{"do you do dents", "do you fix dents"}));
}

TEST_F(PhraseSetScopesTest, AnAgentBuiltInCodeExpandsToo) {
    vasdk::flow::Agent agent;
    agent.set_start_flow("main");
    auto* set = agent.add_phrase_sets();
    set->set_name("on");
    set->add_phrases("(turn|switch) on");
    auto* intent = agent.add_intents();
    intent->set_name("light_on");
    intent->add_examples("${on} the light");
    auto* page = agent.add_flows();
    page->set_name("main");
    page->set_start_page("idle");
    auto* idle = page->add_pages();
    idle->set_name("idle");
    idle->add_routes()->set_intent("light_on");
    std::string error;
    const auto compiled = CompiledAgent::compile(agent, &error);
    ASSERT_NE(compiled, nullptr) << error;
    EXPECT_EQ(compiled->definition().intents(0).examples_size(), 2);
}

TEST_F(PhraseSetScopesTest, TheShippedDevicesPackLoads) {
    const auto pack = std::filesystem::path(VASDK_SOURCE_DIR) / "phrases" / "en-US-devices.txtpb";
    ASSERT_TRUE(std::filesystem::exists(pack)) << pack;
    write("agent.txtpb", mainFile({"light_on"}, "phrase_set_files: [\"" + pack.generic_string() + "\"]\n"
                                                "intents { name: \"light_on\" examples: [\"${devices.on} [the] light\"] }"));
    const auto agent = load();
    ASSERT_NE(agent, nullptr) << _error;
    EXPECT_GE(examples(*agent, "light_on").size(), 4u);
}

} // namespace
