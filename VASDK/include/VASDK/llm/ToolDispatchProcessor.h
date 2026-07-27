#pragma once

#include <VASDK/FrameProcessor.h>
#include <VASDK/llm/Tool.h>
#include <VASDK/llm/PromptTemplate.h>
#include <string>
#include <unordered_map>

namespace va {
namespace llm {

class VASDK_EXPORT ToolDispatchProcessor : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "ToolDispatchProcessor"; }

    void registerTool(Tool tool);
    void unregisterTool(const std::string& name);
    bool hasTool(const std::string& name) const;
    void setToolEnabled(const std::string& name, bool enabled);

    std::vector<std::string> getToolNames() const;

    // Renders the system-prompt section for the currently enabled tools by
    // delegating to the format's PromptTemplate::renderToolPrompt(), which owns
    // the per-format instruction text and its contract with the response parsers.
    std::string renderToolDescriptions(ChatFormat fmt) const;

    // Structured (name/description/parameters) view of the currently enabled tools -
    // each entry is a Tool::toJson(). For UI surfaces (e.g. a "model info" screen) that
    // want to render tools as distinct items rather than parsing renderToolDescriptions()'s
    // format-specific prompt text.
    nlohmann::json getEnabledToolsJson() const;

    void setTextFieldGuidance(const std::string& guidance) { _textFieldGuidance = guidance; }
    const std::string& getTextFieldGuidance() const { return _textFieldGuidance; }

    void handleToolCall(std::shared_ptr<transport::ToolCallFrame> frame);

    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override {
        return frame && frame->getType() == transport::FrameType::TOOL_CALL;
    }

    void processFrame(std::shared_ptr<transport::Frame> frame,
                      pipeline::FlowDirection direction) override;

private:
    std::unordered_map<std::string, Tool> _tools;
    std::string _textFieldGuidance{
        "your reply to the user - include this field ONLY when you are not calling any tools"
    };
};

} // namespace llm
} // namespace va
