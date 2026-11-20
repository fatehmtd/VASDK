// Context-based barge-in on the LLM the session already runs: LlmBargeInClassifier over the
// in-process llama.cpp model (LFM2.5 by default), as Session wires it when barge_in_llm_profile is
// unset and llm has no endpoint. The booking example of docs/reference/processors.md: what the user
// says over the read-back, and while the agent thinks about the request. Skipped without the model.

#include "LlamaCpp/LlamaCppCompletion.h"

#include <VASDK/llm/LlmBargeInClassifier.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace va;
using pipeline::BackchannelVerdict;

namespace {

constexpr const char* kReadBack =
    "I have a table for four on Tuesday at 7 pm, in the garden, under the name Martin. Shall I confirm it?";
constexpr const char* kRequest = "Book a table for Tuesday";

struct Overlap {
    const char* heard;
    bool agentIsThinking;
    BackchannelVerdict expected;
};

const char* nameOf(BackchannelVerdict verdict) {
    switch (verdict) {
    case BackchannelVerdict::Backchannel: return "keep";
    case BackchannelVerdict::Interrupt: return "interrupt";
    default: return "undecided";
    }
}

} // namespace

TEST(LlamaCppBargeIn, TheSessionsModelJudgesTheBookingExample) {
    llm::LLMConfig config;
    config.model = (va::test::modelsDirectory() / VASDK_LLAMACPP_TEST_MODEL_NAME).string();
    VA_REQUIRE_FILE(config.model);
    config.temperature = 0.01f;   // as good as greedy: 0 means the default, 0.7
    config.maxTokens = 16;
    config.nCtx = 2048;
    auto completion = std::make_shared<thirdparty::llm::LlamaCppCompletion>(config);
    ASSERT_TRUE(completion->loaded()) << config.model;
    // The time limit Session gives it by default is 800 ms; here it's room to see every answer.
    llm::LlmBargeInClassifier classifier(completion, 10000);

    const std::vector<Overlap> overlaps = {
        {"mm-hmm", false, BackchannelVerdict::Backchannel},
        {"wait", false, BackchannelVerdict::Interrupt},
        {"sorry, which day?", false, BackchannelVerdict::Interrupt},
        {"Tuesday, right", false, BackchannelVerdict::Backchannel},
        {"yeah yeah that sounds great", false, BackchannelVerdict::Backchannel},
        {"no, I said Wednesday", false, BackchannelVerdict::Interrupt},
        {"okay", true, BackchannelVerdict::Backchannel},
        {"thanks", true, BackchannelVerdict::Backchannel},
        {"for four people", true, BackchannelVerdict::Interrupt},
        {"actually, make it Wednesday", true, BackchannelVerdict::Interrupt},
    };
    size_t correct = 0;
    for (const auto& overlap : overlaps) {
        pipeline::BargeInContext context;
        context.heard = overlap.heard;
        context.isFinal = true;
        context.confidence = 0.9f;
        context.agentIsThinking = overlap.agentIsThinking;
        context.agentReply = overlap.agentIsThinking ? "" : kReadBack;
        context.lastUserTurn = kRequest;
        const auto started = std::chrono::steady_clock::now();
        const auto verdict = classifier.classify(context);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const bool right = verdict == overlap.expected;
        if (right) ++correct;
        std::printf("%s %-9s \"%s\" -> %s in %lld ms\n", right ? "ok  " : "MISS", overlap.agentIsThinking ? "thinking" : "speaking",
                    overlap.heard, nameOf(verdict), static_cast<long long>(ms));
    }
    // A 1.2B model isn't always right (LFM2.5 on macOS: 9 of 10, "okay" while thinking taken as more
    // words). A miss costs little: in MODEL_WHEN_RULES_UNSURE the word rules settle the plain phrases.
    EXPECT_GE(correct, 8u) << "of " << overlaps.size();
}
