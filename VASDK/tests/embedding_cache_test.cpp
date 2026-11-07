// The sentence encoder's vectors of an agent's examples are kept in a file: a later start reads
// them instead of encoding again, another encoder's file is ignored, and a broken file is too.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/nlu/EmbeddingNluProvider.h>

#include <google/protobuf/text_format.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

// A vector per sentence from its letters, normalized; counts how often it's asked.
class CountingEncoder : public nlu::SentenceEncoder {
public:
    explicit CountingEncoder(std::string name) : _name(std::move(name)) {}
    std::string name() const override { return _name; }
    std::vector<float> encode(const std::string& sentence) const override {
        ++calls;
        std::vector<float> vector(26, 0.0f);
        for (const char c : sentence)
            if (c >= 'a' && c <= 'z') vector[static_cast<size_t>(c - 'a')] += 1.0f;
        float length = 0;
        for (const float value : vector) length += value * value;
        length = std::sqrt(length);
        if (length > 0)
            for (auto& value : vector) value /= length;
        return vector;
    }
    mutable std::atomic<int> calls{0};

private:
    std::string _name;
};

} // namespace

int main() {
    vasdk::flow::Agent agent;
    google::protobuf::TextFormat::ParseFromString(R"(
        intents { name: "hours" examples: ["when are you open", "opening hours"] }
        intents { name: "book" examples: ["book a service for @sys.date", "make an appointment"] })", &agent);
    const auto path = (std::filesystem::temp_directory_path() / "vasdk_embedding_cache_test.bin").string();
    std::filesystem::remove(path);

    auto first = std::make_shared<CountingEncoder>("letters-v1");
    nlu::EmbeddingNluProvider encoding(first, agent.intents());
    check(encoding.loadCache(path) == 0, "no file yet: nothing read");
    check(encoding.precompute({"do you sell tires"}) == 5 && first->calls == 5, "the 4 examples and a question, encoded once");
    check(encoding.precompute({"do you sell tires"}) == 0, "and not again");
    std::string error;
    check(encoding.saveCache(path, &error), "saved: " + error);

    // A later start, with the same encoder: read, not encoded.
    auto again = std::make_shared<CountingEncoder>("letters-v1");
    nlu::EmbeddingNluProvider reading(again, agent.intents());
    check(reading.loadCache(path) == 5, "the 5 texts are read");
    check(reading.precompute({"do you sell tires"}) == 0 && again->calls == 0, "nothing to encode");
    nlu::NluRequest request;
    request.text = "when do you open";
    const auto understood = reading.understand(request);
    check(!understood.intents.empty() && understood.intents.front().intent == "hours" && again->calls == 1,
          "it understands with the read vectors (only the user's words are encoded)");

    // Another encoder's file, or a broken one, isn't used.
    auto other = std::make_shared<CountingEncoder>("letters-v2");
    nlu::EmbeddingNluProvider otherModel(other, agent.intents());
    check(otherModel.loadCache(path) == 0 && otherModel.precompute() == 4, "another encoder's file: encoded again");
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "not a cache";
    nlu::EmbeddingNluProvider broken(again, agent.intents());
    check(broken.loadCache(path) == 0, "a broken file: nothing read");
    std::filesystem::remove(path);

    // Sessions with providers.sentence_encoder_cache: the first writes it, the next only reads it.
    {
        const auto models = std::filesystem::temp_directory_path() / "vasdk_embedding_cache_models";
        std::filesystem::create_directories(models);
        auto shared = std::make_shared<CountingEncoder>("letters-v1");
        engine::ProviderRegistry registry;
        registry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
        registry.add<audio::GenericTranscriptionProvider>("stt", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeStt>(); });
        registry.add<audio::GenericVoiceSynthesisProvider>("tts", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeTts>(); });
        registry.add<nlu::SentenceEncoder>("letters", [shared](const engine::ProviderContext&) { return shared; });
        vasdk::engine::EngineConfig config;
        std::string configError;
        engine::parseProto(R"(providers { stt { id: "stt" } tts { id: "tts" } sentence_encoder { id: "letters" }
                                          sentence_encoder_cache: "agent.embeddings" })",
                           engine::ProtoFormat::Text, config, &configError);
        vasdk::flow::Agent definition = agent;
        google::protobuf::TextFormat::MergeFromString(R"(
            start_flow: "main"
            knowledge { questions: ["do you sell tires"] answer: "We do." }
            flows { name: "main" start_page: "p" pages { name: "p" on_enter { ask { say { text: "Hi." } } } } })", &definition);
        auto compiled = flow::CompiledAgent::compile(definition, &configError);
        check(compiled != nullptr, "the agent compiles: " + configError);
        auto session = [&] {
            VAEngine::Options options;
            options.transport = std::make_shared<fakes::FakeTransport>();
            options.agent = compiled;
            options.modelsDir = models.string();
            return VAEngine::create(config, registry, options, &configError);
        };
        auto firstSession = session();
        check(firstSession != nullptr && shared->calls == 5 && std::filesystem::exists(models / "agent.embeddings"),
              "the first session encodes the examples and the question, and writes the file: " + std::to_string(shared->calls));
        firstSession.reset();
        shared->calls = 0;
        auto nextSession = session();
        check(nextSession != nullptr && shared->calls == 0, "the next session reads them: " + std::to_string(shared->calls));
        nextSession.reset();
        std::filesystem::remove_all(models);
    }

    if (failures == 0) std::printf("embedding_cache_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
