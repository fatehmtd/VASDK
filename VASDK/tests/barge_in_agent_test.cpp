// An agent chooses who decides on speech over its replies (barge_in_decision): a part (say) for
// itself, a page for its parts, the agent for all of them. Parts decided differently stay apart,
// and only the main agent file sets the agent-wide choice.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include "TestSupport.h"

#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>

#include <fstream>
#include <string>

using namespace va;

namespace {

const char* kAgent = R"agent(
start_flow: "main"
barge_in_decision: MODEL_WHEN_RULES_UNSURE
flows { name: "main" start_page: "confirm" pages {
  name: "confirm"
  barge_in_decision: MODEL_FOR_EVERY_OVERLAP
  on_enter { say { text: "I have a table for four on Tuesday at 7 pm." } }
  on_enter { say { text: "In the garden." } }
  on_enter { say { text: "Shall I confirm it?" barge_in_decision: WORD_RULES_ONLY } }
} }
)agent";

std::shared_ptr<const flow::CompiledAgent> compileText(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return flow::CompiledAgent::compile(agent, error);
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

} // namespace

TEST(BargeInAgent, APartsChoiceBeatsItsPagesAndAlikePartsJoin) {
    std::string error;
    auto agent = compileText(kAgent, &error);
    ASSERT_NE(agent, nullptr) << error;
    flow::FlowRuntime runtime(agent);
    const auto reply = runtime.start();
    ASSERT_EQ(reply.segments.size(), 2u) << "the two parts the page decides join; the part deciding for itself stays apart";
    EXPECT_EQ(reply.segments[0].text, "I have a table for four on Tuesday at 7 pm. In the garden.");
    ASSERT_TRUE(reply.segments[0].bargeInDecision.has_value()) << "the page's choice";
    EXPECT_EQ(*reply.segments[0].bargeInDecision, vasdk::engine::Backchannel::MODEL_FOR_EVERY_OVERLAP);
    ASSERT_TRUE(reply.segments[1].bargeInDecision.has_value()) << "the part's own choice";
    EXPECT_EQ(*reply.segments[1].bargeInDecision, vasdk::engine::Backchannel::WORD_RULES_ONLY);
    EXPECT_EQ(agent->definition().barge_in_decision(), vasdk::engine::Backchannel::MODEL_WHEN_RULES_UNSURE)
        << "the agent-wide choice, for the session (Session applies it)";
}

TEST(BargeInAgent, APartWithoutAChoiceOnAPageWithoutOneInheritsTheSessions) {
    const std::string pageChoice = "  barge_in_decision: MODEL_FOR_EVERY_OVERLAP\n";
    std::string text = kAgent;
    text.erase(text.find(pageChoice), pageChoice.size());
    std::string error;
    auto agent = compileText(text, &error);
    ASSERT_NE(agent, nullptr) << error;
    flow::FlowRuntime runtime(agent);
    const auto reply = runtime.start();
    ASSERT_EQ(reply.segments.size(), 2u);
    EXPECT_FALSE(reply.segments[0].bargeInDecision.has_value()) << "unset: the session's (the agent's, else the config's)";
}

TEST(BargeInAgent, OnlyTheMainFileSetsTheAgentWideChoice) {
    va::test::UniqueTemporaryDirectory directory("vasdk_barge_in_agent");
    write(directory.path() / "part.txtpb", "barge_in_decision: MODEL_FOR_EVERY_OVERLAP\n");
    write(directory.path() / "main.txtpb", std::string(kAgent) + "include_files: \"part.txtpb\"\n");
    std::string error;
    EXPECT_EQ(flow::CompiledAgent::load((directory.path() / "main.txtpb").string(), &error), nullptr);
    EXPECT_NE(error.find("barge_in_decision: only the main agent file sets these"), std::string::npos) << error;
}
