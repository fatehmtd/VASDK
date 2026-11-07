// GLiNER on the downloaded model: the spans and scores of a Python reference (onnxruntime +
// sentencepiece, the same inputs, the same onnxruntime version), and nothing for words that aren't an entity.
// usage: vasdk_gliner_ner_test <model_dir>; exits 77 (skipped) without the model.

#include "GlinerNer/GlinerEntityExtractor.h"

#include <VASDK/flow/Entities.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace va;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
bool has(const std::vector<nlu::EntitySpan>& spans, const char* entity, size_t start, size_t length, float score) {
    for (const auto& span : spans)
        if (span.entity == entity && span.start == start && span.length == length && std::fabs(span.score - score) < 0.03f) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::filesystem::exists(std::string(argv[1]) + "/model.onnx")) {
        std::printf("no GLiNER model: skipped\n");
        return 77;
    }
    thirdparty::GlinerEntityExtractor gliner(thirdparty::GlinerEntityExtractorOptions{argv[1]});
    if (!gliner.initialize()) return 1;
    const std::vector<nlu::EntityLabel> labels{{"service", "car repair", "", {}}, {"time", "appointment time", "", {}},
                                               {"person", "person", "", {}}, {"place", "location", "", {}}};
    auto spans = [&](const char* text) {
        auto found = gliner.extract(flow::entityWords(text), labels, false);
        for (const auto& span : found)
            std::printf("  %s: @%s %zu+%zu %.3f\n", text, span.entity.c_str(), span.start, span.length, span.score);
        return found;
    };
    // The small model's reference values (onnxruntime 1.19.2, the version linked here: int8 kernels
    // score a little differently from one version to another); the multilingual one scores differently.
    const bool small = std::filesystem::file_size(std::string(argv[1]) + "/model.onnx") < 250'000'000;
    const auto repair = spans("book me a tire repair");
    check(repair.size() == 1 && repair[0].entity == "service" && repair[0].start == 3 && repair[0].length == 2,
          "a tire repair is a car repair");
    if (small) check(has(repair, "service", 3, 2, 0.767f), "with the reference score");
    const auto people = spans("Barack Obama visited Paris");
    check(people.size() == 2 && people[0].entity == "person" && people[0].length == 2 && people[1].entity == "place" &&
              people[1].start == 3, "a person and a place");
    if (small) check(has(people, "person", 0, 2, 0.937f) && has(people, "place", 3, 1, 0.858f), "with the reference scores");
    check(spans("say bananas three times").empty(), "no entity in a sentence without one");
    std::printf("gliner checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
