// An agent's LLM on the device: a GGUF model answers an intent check and a spoken reply in full,
// a time limit gives up, and an agent's turn is understood by it. Exits 77 (skipped) without a
// model.
//   llamacpp_completion_test <model.gguf>

#include "LlamaCppCompletion.h"

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/LlmNluProvider.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::fprintf(ok ? stdout : stderr, "%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::filesystem::exists(argv[1])) {
        std::printf("skipped: no GGUF model (llamacpp_completion_test <model.gguf>)\n");
        return 77;
    }
    va::llm::LLMConfig config;
    config.model = argv[1];
    config.maxTokens = 120;
    config.nCtx = 2048;
    auto llm = std::make_shared<va::thirdparty::llm::LlamaCppCompletion>(config);
    check(llm->loaded(), std::string("the model loads: ") + argv[1]);
    if (!llm->loaded()) return 1;

    const auto started = std::chrono::steady_clock::now();
    const auto answer = llm->complete({{va::llm::MessageRole::SYSTEM, "Reply with one short sentence.", 0},
                                       {va::llm::MessageRole::USER, "Say hello to a customer of a garage.", 0}});
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    check(answer && !answer->empty(), "a reply in " + std::to_string(seconds).substr(0, 4) + " s: " + answer.value_or("(none)"));
    check(!llm->completeWithin({{va::llm::MessageRole::USER, "Write a long story about cars.", 0}}, 1).has_value(),
          "a 1 ms time limit: no answer");

    // An agent asks it what its rules don't understand.
    vasdk::flow::Agent definition;
    google::protobuf::TextFormat::ParseFromString(R"(
        start_flow: "main"
        intents { name: "hours" examples: ["opening hours", "when are you open"] }
        intents { name: "book" examples: ["book a service", "make an appointment"] }
        flows { name: "main" start_page: "p"
          pages { name: "p" on_enter { ask { say { text: "How can I help?" } } }
                  routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
                  routes { intent: "book" steps { say { text: "Let's book it." } } } } })", &definition);
    std::string error;
    auto agent = va::flow::CompiledAgent::compile(definition, &error);
    va::flow::FlowRuntime conversation(agent);
    conversation.setNlu(std::make_shared<va::nlu::CascadeNluProvider>(std::vector<std::shared_ptr<const va::nlu::NluProvider>>{
        agent->rules(), std::make_shared<va::nlu::LlmNluProvider>(llm, agent->definition().intents(), 0)}));
    conversation.start();
    const auto reply = conversation.handleText("my car makes a noise, can I bring it in on friday");
    check(reply.intent == "book", "the model understands a paraphrase as book: " + reply.intent + " \"" + reply.text + "\"");
    std::printf("llamacpp_completion_test: %s\n", failures == 0 ? "all passed" : "failures");
    return failures == 0 ? 0 : 1;
}
