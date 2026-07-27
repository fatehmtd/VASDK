#include <VASDK/llm/ToolDispatchProcessor.h>
#include <VASDK/Frame.h>
#include <spdlog/spdlog.h>
#include <thread>

namespace va {
namespace llm {

void ToolDispatchProcessor::registerTool(Tool tool) {
    _tools[tool.name()] = std::move(tool);
}

void ToolDispatchProcessor::unregisterTool(const std::string& name) {
    _tools.erase(name);
}

bool ToolDispatchProcessor::hasTool(const std::string& name) const {
    return _tools.count(name) > 0;
}

void ToolDispatchProcessor::setToolEnabled(const std::string& name, bool enabled) {
    auto it = _tools.find(name);
    if (it != _tools.end()) {
        it->second.setEnabled(enabled);
    }
}

std::string ToolDispatchProcessor::renderToolDescriptions(ChatFormat fmt) const {
    std::vector<const Tool*> enabledTools;
    enabledTools.reserve(_tools.size());
    for (const auto& [name, tool] : _tools) {
        if (tool.isEnabled()) {
            enabledTools.push_back(&tool);
        }
    }
    if (enabledTools.empty()) return {};

    return PromptTemplate::create(fmt)->renderToolPrompt(enabledTools, _textFieldGuidance);
}

nlohmann::json ToolDispatchProcessor::getEnabledToolsJson() const {
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& [name, tool] : _tools) {
        if (tool.isEnabled()) tools.push_back(tool.toJson());
    }
    return tools;
}

void ToolDispatchProcessor::handleToolCall(
    std::shared_ptr<transport::ToolCallFrame> frame) {
    enqueueFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
}

void ToolDispatchProcessor::processFrame(
    std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) {

    if (direction != pipeline::FlowDirection::DOWNSTREAM) return;

    auto toolFrame = std::dynamic_pointer_cast<transport::ToolCallFrame>(frame);
    if (!toolFrame) {
        pushDownstream(frame);
        return;
    }

    const std::string& name = toolFrame->toolName;

    auto sendErrorResult = [&](const std::string& errorMessage) {
        auto resultFrame        = std::make_shared<transport::ToolResultFrame>();
        resultFrame->toolName   = name;
        resultFrame->resultJson = nlohmann::json{{"error", errorMessage}}.dump();
        resultFrame->timestamp  = transport::Frame::getCurrentTimestamp();
        resultFrame->sourceId   = getName();
        pushUpstream(resultFrame);
    };

    auto it = _tools.find(name);
    if (it == _tools.end() || !it->second.isEnabled()) {
        spdlog::warn("[ToolDispatch] Tool '{}' is disabled or unregistered - ignoring", name);
        sendErrorResult("unknown tool: " + name);
        return;
    }

    nlohmann::json args;
    try {
        args = nlohmann::json::parse(toolFrame->argsJson);
    } catch (const std::exception& e) {
        spdlog::error("[ToolDispatch] Failed to parse args for '{}': {}", name, e.what());
        sendErrorResult(std::string("invalid arguments: ") + e.what());
        return;
    }

    spdlog::info("[ToolDispatch] Calling '{}' args={}", name, args.dump());
    std::thread([this, name, args = std::move(args)]() mutable {
        nlohmann::json result = _tools.at(name).execute(args);
        spdlog::info("[ToolDispatch] '{}' returned: {}", name, result.dump());

        auto resultFrame        = std::make_shared<transport::ToolResultFrame>();
        resultFrame->toolName   = name;
        resultFrame->resultJson = result.dump();
        resultFrame->timestamp  = transport::Frame::getCurrentTimestamp();
        resultFrame->sourceId   = getName();

        pushUpstream(resultFrame);
    }).detach();
}

    std::vector<std::string> ToolDispatchProcessor::getToolNames() const {
        std::vector<std::string> toolNames;
        for (const auto& tool : _tools) {
            toolNames.push_back(tool.first);
        }
        return toolNames;
    }

} // namespace llm
} // namespace va
