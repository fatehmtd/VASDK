#pragma once

#include "vasdk_export.h"
#include "CompiledAgent.h"
#include "FlowRuntime.h"
#include "vasdk/flow/test_case.pb.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace flow {

struct FlowTestResult {
    std::string name;
    std::vector<std::string> failures;   // "turn 2 \"book\": page is \"welcome\", expected \"booking\""
    bool passed() const { return failures.empty(); }
};

/** Sets up each test's conversation before it starts: an NLU, an LLM (FlowRuntime::setNlu, setLlm). */
using FlowTestSetup = std::function<void(FlowRuntime& conversation)>;

/** Plays the case's turns, by text, through a fresh FlowRuntime and checks each expectation. */
VASDK_EXPORT FlowTestResult runFlowTest(std::shared_ptr<const CompiledAgent> agent,
                                        const vasdk::flow::FlowTestCase& testCase, const FlowTestSetup& setup = nullptr);
/** Loads a .txtpb/.json/.pb FlowTestCase file, then runFlowTest(); a load error is a failure. */
VASDK_EXPORT FlowTestResult runFlowTestFile(std::shared_ptr<const CompiledAgent> agent, const std::string& path,
                                            const FlowTestSetup& setup = nullptr);

} // namespace flow
} // namespace va
