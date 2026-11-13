// The MiniLM sentence encoder after an agent's rules: paraphrases the examples miss are understood
// (the eval gains over the rules alone), and a knowledge question is answered by its meaning.
// Skipped without the model: cmake/DownloadMiniLmModel.cmake fetches it.

#include "MiniLmNlu/MiniLmSentenceEncoder.h"

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include "TestSupport.h"

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace va;

namespace {

std::string agentText() {
    return R"agent(
start_flow: "main"
intents { name: "book" examples: ["book an appointment", "bring my car in", "schedule a service"] }
intents { name: "hours" examples: ["opening hours", "when are you open"] }
intents { name: "price" examples: ["how much does it cost", "price"] }
intents { name: "person" examples: ["talk to a person", "speak to someone"] }
knowledge { questions: ["do you do bodywork", "can you fix dents"] answer: "We don't do bodywork." }
flows { name: "main" start_page: "p" pages {
  name: "p" on_enter { ask { say { text: "How can I help?" } } }
  routes { intent: "book" } routes { intent: "hours" } routes { intent: "price" } routes { intent: "person" } } }
)agent";
}

// The encoder, loaded from the model vasdk_add_gtest(... MODELS_DIR) names; skipped without it.
class MiniLmSentenceEncoderTest : public testing::Test {
protected:
    void SetUp() override {
        const auto modelDirectory = va::test::modelsDirectory();
        VA_REQUIRE_FILE(modelDirectory / "model.onnx");
        encoder = std::make_shared<thirdparty::nlu::MiniLmSentenceEncoder>(
            thirdparty::nlu::MiniLmSentenceEncoderOptions{modelDirectory.string()});
        ASSERT_TRUE(encoder->initialize()) << "the model loads";
    }

    std::shared_ptr<thirdparty::nlu::MiniLmSentenceEncoder> encoder;
};

// The agent and its rules, then the rules followed by the encoder.
class MiniLmAgentTest : public MiniLmSentenceEncoderTest {
protected:
    void SetUp() override {
        MiniLmSentenceEncoderTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) return;
        ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(agentText(), &definition)) << "the agent's text parses";
        std::string error;
        agent = flow::CompiledAgent::compile(definition, &error);
        ASSERT_NE(agent, nullptr) << "the agent compiles: " << error;
        embeddings = std::make_shared<va::nlu::EmbeddingNluProvider>(encoder, definition.intents());
        cascade = std::make_shared<va::nlu::CascadeNluProvider>(
            std::vector<std::shared_ptr<const va::nlu::NluProvider>>{agent->rules(), embeddings});
    }

    vasdk::flow::Agent definition;
    std::shared_ptr<const flow::CompiledAgent> agent;
    std::shared_ptr<va::nlu::EmbeddingNluProvider> embeddings;
    std::shared_ptr<va::nlu::CascadeNluProvider> cascade;
};

TEST_F(MiniLmSentenceEncoderTest, TokenizesWithWordPiece) {
    EXPECT_EQ(encoder->tokenize("Hello, world!").size(), 6u) << "WordPiece: [CLS] hello , world ! [SEP]";
}

TEST_F(MiniLmSentenceEncoderTest, EncodesA384NumberVector) {
    EXPECT_EQ(encoder->encode("is the shop open on saturday").size(), 384u);
}

TEST_F(MiniLmSentenceEncoderTest, PutsSimilarSentencesCloser) {
    const auto open = encoder->encode("when are you open");
    EXPECT_GT(va::nlu::cosine(open, encoder->encode("what time do you close")),
              va::nlu::cosine(open, encoder->encode("I want to book a service")));
}

// Paraphrases: the rules alone against the rules followed by the model.
TEST_F(MiniLmAgentTest, UnderstandsParaphrasesTheRulesMiss) {
    const std::vector<std::pair<std::string, std::string>> said{
        {"is the shop open on saturday", "hours"},   {"what time do you close", "hours"},
        {"I'd like to get my car serviced", "book"}, {"can you fit me in next week", "book"},
        {"what would that set me back", "price"},    {"is it expensive", "price"},
        {"I need a human", "person"},                {"can I get a real person on the line", "person"},
        {"book an appointment", "book"},             {"opening hours please", "hours"},
    };
    auto accuracy = [&](const std::shared_ptr<const va::nlu::NluProvider>& provider) {
        int right = 0;
        for (const auto& [text, intent] : said) {
            va::nlu::NluRequest request;
            request.text = text;
            const auto understood = provider->understand(request);
            if (!understood.intents.empty() && understood.intents.front().intent == intent) ++right;
        }
        return static_cast<double>(right) / static_cast<double>(said.size());
    };
    const double rules = accuracy(agent->rules()), withModel = accuracy(cascade);
    EXPECT_GT(withModel, rules) << "the model gains over the rules";
    EXPECT_GE(withModel, 0.7) << "rules + MiniLM understand most paraphrases";
}

TEST_F(MiniLmAgentTest, AnswersAKnowledgeQuestionByMeaning) {
    flow::FlowRuntime conversation(agent);
    conversation.setNlu(cascade);
    conversation.setEmbeddings(embeddings);
    conversation.start();
    const auto reply = conversation.handleText("could you repair a dent in my door");
    EXPECT_EQ(reply.text.rfind("We don't do bodywork.", 0), 0u) << "a knowledge question by meaning: " << reply.text;
}

} // namespace
