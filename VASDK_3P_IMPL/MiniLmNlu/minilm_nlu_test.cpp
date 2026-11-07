// The MiniLM sentence encoder after an agent's rules: paraphrases the examples miss are understood
// (the eval gains over the rules alone), and a knowledge question is answered by its meaning.
// Skipped (77) without the model: cmake/DownloadMiniLmModel.cmake fetches it.

#include "MiniLmNlu/MiniLmSentenceEncoder.h"

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

const char* kAgent = R"agent(
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

} // namespace

int main(int argc, char** argv) {
    const std::string modelDir = argc > 1 ? argv[1] : "models/minilm";
    if (!std::filesystem::exists(modelDir + "/model.onnx")) {
        std::printf("minilm_nlu_test: skipped, no model in %s\n", modelDir.c_str());
        return 77;
    }
    auto encoder = std::make_shared<thirdparty::MiniLmSentenceEncoder>(thirdparty::MiniLmSentenceEncoderOptions{modelDir});
    check(encoder->initialize(), "the model loads");
    check(encoder->tokenize("Hello, world!").size() == 6, "WordPiece: [CLS] hello , world ! [SEP]");
    const auto vector = encoder->encode("is the shop open on saturday");
    check(vector.size() == 384, "a 384-number vector");
    check(nlu::cosine(encoder->encode("when are you open"), encoder->encode("what time do you close")) >
              nlu::cosine(encoder->encode("when are you open"), encoder->encode("I want to book a service")),
          "similar sentences are closer");

    vasdk::flow::Agent definition;
    (void)google::protobuf::TextFormat::ParseFromString(kAgent, &definition);
    std::string error;
    auto agent = flow::CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;
    auto embeddings = std::make_shared<nlu::EmbeddingNluProvider>(encoder, definition.intents());
    auto cascade = std::make_shared<nlu::CascadeNluProvider>(
        std::vector<std::shared_ptr<const nlu::NluProvider>>{agent->rules(), embeddings});

    // Paraphrases: the rules alone against the cascade.
    const std::vector<std::pair<std::string, std::string>> said{
        {"is the shop open on saturday", "hours"},   {"what time do you close", "hours"},
        {"I'd like to get my car serviced", "book"}, {"can you fit me in next week", "book"},
        {"what would that set me back", "price"},    {"is it expensive", "price"},
        {"I need a human", "person"},                {"can I get a real person on the line", "person"},
        {"book an appointment", "book"},             {"opening hours please", "hours"},
    };
    auto accuracy = [&](const std::shared_ptr<const nlu::NluProvider>& provider) {
        int right = 0;
        for (const auto& [text, intent] : said) {
            nlu::NluRequest request;
            request.text = text;
            const auto understood = provider->understand(request);
            if (!understood.intents.empty() && understood.intents.front().intent == intent) ++right;
        }
        return static_cast<double>(right) / static_cast<double>(said.size());
    };
    const double rules = accuracy(agent->rules()), withModel = accuracy(cascade);
    std::printf("intent accuracy: rules %.0f%%, rules + minilm %.0f%%\n", rules * 100, withModel * 100);
    check(withModel > rules && withModel >= 0.7, "the model gains over the rules");

    // A knowledge question by meaning.
    flow::FlowRuntime conversation(agent);
    conversation.setNlu(cascade);
    conversation.setEmbeddings(embeddings);
    conversation.start();
    const auto reply = conversation.handleText("could you repair a dent in my door");
    check(reply.text.rfind("We don't do bodywork.", 0) == 0, "a knowledge question by meaning: " + reply.text);

    if (failures == 0) std::printf("minilm_nlu_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
