// The call center's test calls pass the same with and without an LLM: an LLM after the rules
// (understanding and form answers, as a session with an `llm` endpoint has it) changes nothing
// the agent understands itself. Here the LLM is scripted and never sure; the same suite runs
// against a real one with: callcenter_agentc test agent.txtpb tests/*.txtpb --llm

#include "CallCenterTools.h"

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowTestRunner.h>
#include <VASDK/llm/Completion.h>
#include <VASDK/nlu/LlmNluProvider.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct UnsureLlm : va::llm::Completion {
    mutable std::atomic<int> calls{0};
    std::optional<std::string> complete(const std::vector<va::llm::ConversationMessage>&) const override {
        ++calls;
        return std::string(R"({"intent": "none", "slots": {}})");
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: callcenter_suite_test <agent dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    callcenter::registerTools(*registry);
    std::string error;
    auto agent = va::flow::CompiledAgent::load((dir / "agent.txtpb").string(), &error, nullptr, registry);
    if (!agent) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "tests"))
        if (entry.path().extension() == ".txtpb") cases.push_back(entry.path().string());

    auto llm = std::make_shared<UnsureLlm>();
    auto nlu = std::make_shared<va::nlu::CascadeNluProvider>(std::vector<std::shared_ptr<const va::nlu::NluProvider>>{
        agent->rules(), std::make_shared<va::nlu::LlmNluProvider>(llm, agent->definition().intents())});
    int failed = 0;
    for (const bool online : {false, true}) {
        for (const auto& path : cases) {
            const auto result = va::flow::runFlowTestFile(agent, path, online ? va::flow::FlowTestSetup([&](va::flow::FlowRuntime& call) {
                call.setNlu(nlu);
                call.setLlm(llm);
            }) : nullptr);
            if (result.passed()) continue;
            ++failed;
            std::printf("FAIL (%s) %s\n", online ? "with an LLM" : "offline", result.name.c_str());
            for (const auto& failure : result.failures) std::printf("    %s\n", failure.c_str());
        }
    }
    if (llm->calls == 0) {
        std::printf("FAIL the LLM was never asked: the online run tested nothing\n");
        ++failed;
    }
    std::printf("%zu cases, offline and with an LLM (asked %d times): %s\n", cases.size(), llm->calls.load(),
                failed ? "failures" : "all passed");
    return failed ? 1 : 0;
}
