// ToolDispatchProcessor under concurrency: tools are registered/unregistered while other threads
// read and execute them, and a tool handler registers tools itself (as switch_goal does) without
// deadlocking. Run the binary under -fsanitize=thread to catch data races.

#include <VASDK/llm/ToolDispatchProcessor.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

llm::Tool echoTool(const std::string& name) {
    return llm::Tool(name, "echo").onCall([name](const nlohmann::json&) -> nlohmann::json {
        return {{"tool", name}};
    });
}

} // namespace

int main() {
    llm::ToolDispatchProcessor dispatcher;
    dispatcher.registerTool(echoTool("stable"));

    // A handler that re-registers tools from inside its own call must not deadlock.
    dispatcher.registerTool(llm::Tool("switch", "re-registers tools").onCall(
        [&dispatcher](const nlohmann::json&) -> nlohmann::json {
            dispatcher.unregisterTool("dynamic");
            dispatcher.registerTool(echoTool("dynamic"));
            return {{"switched", true}};
        }));
    check(dispatcher.executeTool("switch", {}).value("switched", false), "handler can register tools");
    check(dispatcher.hasTool("dynamic"), "tool registered from a handler is visible");

    std::atomic<bool> stop{false};
    std::atomic<int> stableErrors{0};
    std::vector<std::thread> threads;
    for (int writer = 0; writer < 2; ++writer) {
        threads.emplace_back([&, writer] {
            for (int i = 0; !stop; ++i) {
                auto name = "t" + std::to_string(writer) + "_" + std::to_string(i % 16);
                dispatcher.registerTool(echoTool(name));
                dispatcher.setToolEnabled(name, i % 2 == 0);
                dispatcher.unregisterTool(name);
            }
        });
    }
    for (int reader = 0; reader < 3; ++reader) {
        threads.emplace_back([&] {
            while (!stop) {
                if (dispatcher.executeTool("stable", {}).value("tool", "") != "stable") ++stableErrors;
                dispatcher.executeTool("switch", {});
                dispatcher.getToolNames();
                dispatcher.renderToolDescriptions(llm::ChatFormat::OPENAI);
                dispatcher.getEnabledToolsJson();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop = true;
    for (auto& t : threads) t.join();

    check(stableErrors == 0, "a stable tool always executes while others churn");
    check(dispatcher.executeTool("missing", {}).contains("error"), "unknown tool returns an error");

    if (failures == 0) std::printf("tool_dispatch_race_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
