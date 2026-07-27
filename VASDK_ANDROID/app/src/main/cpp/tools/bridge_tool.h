#pragma once

#include <VASDK/llm/Tool.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <spdlog/spdlog.h>

using AndroidToolBridge = std::function<std::string(const std::string& toolName, const std::string& argsJson)>;

// Base for every tool that delegates to Kotlin via the JNI bridge.
// Subclasses call addParameter() in their own constructor body; onCall is
// already wired here so the bridge is invoked automatically.
class BridgeTool : public va::llm::Tool {
public:
    BridgeTool(const std::string& name, std::string desc, AndroidToolBridge bridge)
        : Tool(name, std::move(desc))
    {
        onCall([b = std::move(bridge), n = name](const nlohmann::json& args) -> nlohmann::json {
            try {
                return nlohmann::json::parse(b(n, args.dump()));
            } catch (const std::exception& e) {
                spdlog::error("[{}] bridge parse error: {}", n, e.what());
                return nlohmann::json{{"error", "bridge parse failed"}};
            }
        });
    }
};
