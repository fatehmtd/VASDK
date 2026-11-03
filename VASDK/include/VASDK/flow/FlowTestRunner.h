#pragma once

#include "vasdk_export.h"
#include "CompiledAgent.h"
#include "vasdk/flow/test_case.pb.h"

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

/** Plays the case's turns, by text, through a fresh FlowRuntime and checks each expectation. */
VASDK_EXPORT FlowTestResult runFlowTest(std::shared_ptr<const CompiledAgent> agent,
                                        const vasdk::flow::FlowTestCase& testCase);
/** Loads a .txtpb/.json/.pb FlowTestCase file, then runFlowTest(); a load error is a failure. */
VASDK_EXPORT FlowTestResult runFlowTestFile(std::shared_ptr<const CompiledAgent> agent, const std::string& path);

} // namespace flow
} // namespace va
