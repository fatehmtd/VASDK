// Entity models (nlu::EntityExtractor): with spans, a list entity's misspellings count only inside
// the words a model marked as it (looser there: only which value is left), exact values anywhere.
// The LLM extractor keeps only words that are in the sentence and never sees a sensitive answer;
// the tagger learns from the agent's placeholder examples; FlowRuntime asks the extractor and,
// with exact rules, matches intents without misspellings.

#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/LlmEntityExtractor.h>
#include <VASDK/nlu/TaggerEntityExtractor.h>

#include <atomic>
#include <cstdio>
#include <functional>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

const char* kAgent = R"(
  start_flow: "main"
  entity_types { name: "service" list {
    values { value: "tire_repair" spoken: "a tire repair" synonyms: ["tires", "tyres"] }
    values { value: "oil_change" spoken: "an oil change" synonyms: ["oil change"] } } }
  intents { name: "book" examples: ["book @service", "i need @service", "my @service please"] }
  intents { name: "cancel" examples: ["cancel my appointment"] }
  intents { name: "hello" examples: ["hello", "say something", "what time is it"] }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome"
      routes { intent: "cancel" steps { say { text: "Cancelled." } } }
      routes { entity: "@service" steps { say { text: "Booking $entity." } } } } }
)";

std::shared_ptr<const flow::CompiledAgent> compile() {
    vasdk::flow::Agent agent;
    std::string error;
    if (!engine::parseProto(kAgent, engine::ProtoFormat::Text, agent, &error)) std::fprintf(stderr, "parse: %s\n", error.c_str());
    std::vector<std::string> warnings;
    auto compiled = flow::CompiledAgent::compile(agent, &error, &warnings);
    if (!compiled) std::fprintf(stderr, "compile: %s\n", error.c_str());
    return compiled;
}

struct FixedExtractor : nlu::EntityExtractor {
    std::function<std::vector<nlu::EntitySpan>(const std::vector<std::string>&)> spans;
    mutable std::atomic<int> calls{0};
    std::string name() const override { return "fixed"; }
    std::vector<nlu::EntitySpan> extract(const std::vector<std::string>& words, const std::vector<nlu::EntityLabel>&, bool) const override {
        ++calls;
        return spans(words);
    }
};

struct FakeLlm : llm::Completion {
    std::string reply;
    mutable std::atomic<int> calls{0};
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>&) const override {
        ++calls;
        return reply;
    }
};

// A vector per word that depends only on the word: what the tagger has to learn from words alone.
struct WordEncoder : nlu::SentenceEncoder {
    std::string name() const override { return "words"; }
    std::vector<float> encode(const std::string&) const override { return {}; }
    std::vector<std::vector<float>> encodeWords(const std::vector<std::string>& words) const override {
        std::vector<std::vector<float>> vectors;
        for (const auto& word : words) {
            std::vector<float> vector(64, 0.0f);
            vector[std::hash<std::string>{}(word) % 64] = 1.0f;
            vectors.push_back(std::move(vector));
        }
        return vectors;
    }
};

} // namespace

int main() {
    {
        const auto kept = nlu::nonOverlapping({{"a", 0, 2, 0.6f}, {"b", 1, 2, 0.9f}, {"c", 3, 1, 0.7f}});
        check(kept.size() == 2 && kept[0].entity == "b" && kept[1].entity == "c", "the best spans that don't overlap, in order");
    }
    const auto agent = compile();
    if (!agent) return 1;
    const auto& entities = agent->entities();
    const auto now = std::chrono::system_clock::now();
    {
        const std::vector<nlu::EntitySpan> none;
        check(entities.find("service", "say bananas three times").has_value(), "without a model: \"times\" is \"tires\" (one letter off)");
        check(!entities.find("service", "say bananas three times", {}, now, &none).has_value(), "with a model that marks nothing: it isn't");
        check(entities.find("service", "my tires are flat", {}, now, &none).has_value(), "an exact synonym counts anywhere");
        const std::vector<nlu::EntitySpan> marked{{"service", 3, 1, 0.9f}};
        const auto inSpan = entities.find("service", "i need my tyers checked", {}, now, &marked);
        check(inSpan && flow::toText(inSpan->value) == "tire_repair", "inside its span a looser likeness resolves the value");
        check(!entities.find("service", "i need my tyers checked", {}, now, &none).has_value(), "outside any span it doesn't");
        const std::vector<nlu::EntitySpan> other{{"color", 3, 1, 0.9f}};
        check(!entities.find("service", "i need my tyers checked", {}, now, &other).has_value(), "nor in another entity's span");
        const auto labels = entities.modelLabels();
        check(labels.size() == 1 && labels[0].entity == "service" && labels[0].label == "service" &&
                  labels[0].examples.front() == "a tire repair", "labels: the list entities, a few values as said");
    }
    {
        auto llm = std::make_shared<FakeLlm>();
        llm->reply = R"({"entities": [{"entity": "service", "words": "Tyers"}, {"entity": "service", "words": "brakes"},
                                      {"entity": "unknown", "words": "my"}]})";
        nlu::LlmEntityExtractor extractor(llm, 0);
        const auto spans = extractor.extract(flow::entityWords("i need my tyers checked"), entities.modelLabels(), false);
        check(spans.size() == 1 && spans[0].entity == "service" && spans[0].start == 3 && spans[0].length == 1,
              "the LLM's words, where they are in the sentence; made-up words and unknown kinds dropped");
        const int asked = llm->calls;
        check(extractor.extract(flow::entityWords("4111 1111"), entities.modelLabels(), true).empty() && llm->calls == asked,
              "a sensitive answer never reaches the LLM");
    }
    {
        std::vector<std::string> examples;
        for (const auto& intent : agent->definition().intents()) examples.insert(examples.end(), intent.examples().begin(), intent.examples().end());
        nlu::TaggerEntityExtractor tagger(std::make_shared<WordEncoder>(), entities.modelLabels(), examples);
        check(tagger.trained() && tagger.sentences() > 4, "the tagger trains on the values and the filled examples");
        const auto words = flow::entityWords("book an oil change");
        const auto spans = tagger.extract(words, entities.modelLabels(), false);
        check(spans.size() == 1 && spans[0].entity == "service" && spans[0].start == 1 && spans[0].length == 3, "it tags the service");
        check(tagger.extract(flow::entityWords("say something"), entities.modelLabels(), false).empty(), "and not other words");
        nlu::TaggerEntityExtractor untrained(nullptr, entities.modelLabels(), examples);
        check(!untrained.trained() && untrained.extract(words, entities.modelLabels(), false).empty(), "no encoder: it finds nothing");
    }
    {
        flow::FlowRuntime conversation(agent);
        auto extractor = std::make_shared<FixedExtractor>();
        extractor->spans = [](const std::vector<std::string>& words) {
            std::vector<nlu::EntitySpan> spans;
            for (size_t i = 0; i < words.size(); ++i)
                if (words[i] == "tyers") spans.push_back({"service", i, 1, 0.9f});
            return spans;
        };
        conversation.setEntityExtractor(extractor);
        check(conversation.handleText("i need my tyers checked").text == "Booking a tire repair.", "a route's entity, through the model");
        check(conversation.handleText("say bananas three times").text != "Booking a tire repair.", "\"times\" isn't a service any more");
        check(extractor->calls >= 2, "the model is asked about each turn's words");
        flow::FlowRuntime fuzzy(agent), exact(agent);
        exact.setExactRules(true);
        check(fuzzy.handleText("cancel my apointment").text == "Cancelled.", "the rules take a misspelling");
        check(exact.handleText("cancel my apointment").text != "Cancelled.", "exact rules don't: the intent model's job");
    }
    std::printf("entity model checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
