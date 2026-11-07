// Fuzz targets for what reads untrusted text: agent files (text proto, then the load checks),
// expressions and reply templates, and the English value extractors and NLU on what users say.
// Built with libFuzzer (VASDK_BUILD_FUZZERS, clang) each is its own fuzzer; otherwise
// fuzz_smoke_main.cpp runs them on seeds and mutations of them, as a ctest.

#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/Entities.h>
#include <VASDK/flow/Expression.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/Language.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

std::string text(const uint8_t* data, size_t size) { return std::string(reinterpret_cast<const char*>(data), size); }

} // namespace

// An agent file: parse it, check and compile it, and play one turn when it loads.
extern "C" int fuzzAgent(const uint8_t* data, size_t size) {
    vasdk::flow::Agent agent;
    if (!va::engine::parseProto(text(data, size), va::engine::ProtoFormat::Text, agent)) return 0;
    std::string error;
    auto compiled = va::flow::CompiledAgent::compile(agent, &error);
    if (!compiled) return 0;
    va::flow::FlowRuntime conversation(compiled);
    conversation.start();
    conversation.handleText("book tuesday at 3 pm");
    return 0;
}

// An expression and a reply template, evaluated with every reference reading a value.
extern "C" int fuzzExpression(const uint8_t* data, size_t size) {
    const std::string source = text(data, size);
    auto lookup = [](const va::flow::Path& path) { return va::flow::stringValue(path.empty() ? "" : path.back()); };
    if (auto expression = va::flow::Expression::parse(source)) (void)expression->evaluate(lookup);
    std::vector<va::flow::Path> references;
    if (va::flow::templateReferences(source, references)) (void)va::flow::renderTemplate(source, lookup);
    return 0;
}

// What a user says: every system entity, made speakable, and the built-in intents.
extern "C" int fuzzWords(const uint8_t* data, size_t size) {
    const auto english = va::nlu::Language::builtIn();
    const va::flow::EntityCatalog catalog(std::make_shared<const va::nlu::Languages>(va::nlu::Languages({english})));
    const std::string said = text(data, size);
    for (const char* entity : va::flow::kSystemEntities) (void)catalog.find(entity, said);
    (void)catalog.findAll(said);
    (void)english->speakable(said);
    return 0;
}

#ifdef VASDK_FUZZ_TARGET
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) { return VASDK_FUZZ_TARGET(data, size); }
#endif
