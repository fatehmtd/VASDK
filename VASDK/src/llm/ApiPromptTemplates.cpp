#include <VASDK/llm/ApiPromptTemplates.h>
#include <VASDK/llm/Tool.h>

#ifdef VASDK_HAS_JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

namespace va {
namespace llm {

//  Shared tool prompt (plain string composition - no VASDK_HAS_JSON dependency)

std::string ApiPromptTemplate::renderToolPrompt(
        const std::vector<const Tool*>& enabledTools,
        const std::string& textFieldGuidance) const {
    if (enabledTools.empty()) return {};

    std::string toolList;
    for (const Tool* tool : enabledTools) {
        toolList += tool->toPromptDescription();
        toolList += "\n";
    }

    return
        std::string("RESPONSE FORMAT - always reply with a single JSON object, nothing else. "
        "The object contains EITHER \"text\" OR \"tools\", never both:\n"
        "{\n"
        "  \"text\": \"") + textFieldGuidance + "\",\n"
        "  \"tools\": [\n"
        "    { \"tool\": \"tool_name\", \"args\": { ... } }\n"
        "  ]\n"
        "}\n"
        "When calling tools, omit \"text\" entirely; when no tools are needed, omit \"tools\" entirely. "
        "You MAY list multiple tools to run them in parallel. "
        "Only use \"tool\" names that appear verbatim in the list below - never invent one. "
        "After tool results are provided, answer in \"text\" only - "
        "never repeat a tool call whose result you already have.\n\n"
        "Available tools ('*' = required param, '?' = optional):\n" + toolList;
}

//  OpenAI

std::string OpenAIPromptTemplate::render() const {
#ifndef VASDK_HAS_JSON
    throw PromptTemplateError(
        "ChatFormat::OPENAI requires nlohmann_json; rebuild VASDK with nlohmann_json available");
#else
    auto msgs = resolveAll();
    json arr = json::array();
    for (const auto& msg : msgs) {
        json obj;
        switch (msg.role) {
            case MessageRole::SYSTEM:    obj["role"] = "system";    break;
            case MessageRole::USER:      obj["role"] = "user";      break;
            case MessageRole::ASSISTANT: obj["role"] = "assistant"; break;
            case MessageRole::TOOL:      obj["role"] = "tool";      break;
        }
        obj["content"] = msg.content;
        arr.push_back(obj);
    }
    return arr.dump(2);
#endif
}

//  Anthropic

std::string AnthropicPromptTemplate::render() const {
#ifndef VASDK_HAS_JSON
    throw PromptTemplateError(
        "ChatFormat::ANTHROPIC requires nlohmann_json; rebuild VASDK with nlohmann_json available");
#else
    auto msgs = resolveAll();
    std::string systemContent;
    json messagesArr = json::array();

    for (const auto& msg : msgs) {
        if (msg.role == MessageRole::SYSTEM) {
            if (!systemContent.empty()) systemContent += "\n\n";
            systemContent += msg.content;
        } else {
            json obj;
            switch (msg.role) {
                case MessageRole::USER:
                case MessageRole::TOOL:      obj["role"] = "user";      break;
                case MessageRole::ASSISTANT: obj["role"] = "assistant"; break;
                default:                     obj["role"] = "user";      break;
            }
            obj["content"] = msg.content;
            messagesArr.push_back(obj);
        }
    }

    json payload;
    if (!systemContent.empty()) {
        payload["system"] = systemContent;
    }
    payload["messages"] = messagesArr;
    return payload.dump(2);
#endif
}

//  Google

std::string GooglePromptTemplate::render() const {
#ifndef VASDK_HAS_JSON
    throw PromptTemplateError(
        "ChatFormat::GOOGLE requires nlohmann_json; rebuild VASDK with nlohmann_json available");
#else
    auto msgs = resolveAll();
    std::string systemContent;
    json contentsArr = json::array();

    for (const auto& msg : msgs) {
        if (msg.role == MessageRole::SYSTEM) {
            if (!systemContent.empty()) systemContent += "\n\n";
            systemContent += msg.content;
        } else {
            json part;
            part["text"] = msg.content;
            json obj;
            const bool isModel = (msg.role == MessageRole::ASSISTANT);
            obj["role"] = isModel ? "model" : "user";
            obj["parts"] = json::array({part});
            contentsArr.push_back(obj);
        }
    }

    json payload;
    if (!systemContent.empty()) {
        json sysPart;
        sysPart["text"] = systemContent;
        payload["system_instruction"]["parts"] = json::array({sysPart});
    }
    payload["contents"] = contentsArr;
    return payload.dump(2);
#endif
}

} // namespace llm
} // namespace va
