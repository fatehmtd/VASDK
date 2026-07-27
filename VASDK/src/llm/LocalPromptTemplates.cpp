#include <VASDK/llm/LocalPromptTemplates.h>
#include <VASDK/llm/Tool.h>
#include <spdlog/spdlog.h>

namespace va {
namespace llm {

namespace {
// Call-format instruction for the LFM tool prompts. Sourced from the
// format's toolCallTags() so the instruction text always matches what
// LlamaCppTransport watches for when detecting a tool call in the model's output.
std::string lfmCallInstruction(const std::pair<std::string, std::string>& tags) {
    return
        "When you need to call a tool, output EXACTLY:\n" +
        tags.first +
        "[{\"name\": \"tool_name\", \"arguments\": {\"param\": \"value\"}}]" +
        tags.second + "\n"
        "You may include multiple tools in the array to call them in parallel. "
        "When calling tools, output ONLY the tool call block - no text before or after it.";
}

// Shared by the [INST]-style formats (Llama2, Mistral): sorts messages into the
// three buckets their render() alternates over turn-by-turn.
void bucketMessagesForInstFormat(const std::vector<ConversationMessage>& msgs,
                                  std::string& systemContent,
                                  std::vector<std::string>& userTurns,
                                  std::vector<std::string>& assistantTurns) {
    for (const auto& msg : msgs) {
        switch (msg.role) {
        case MessageRole::SYSTEM:
            if (!systemContent.empty()) systemContent += "\n";
            systemContent += msg.content;
            break;
        case MessageRole::USER:
        case MessageRole::TOOL:
            userTurns.push_back(msg.content);
            break;
        default:
            assistantTurns.push_back(msg.content);
            break;
        }
    }
}
} // namespace

//  ChatML

std::string ChatMLPromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string out;
    for (const auto& msg : msgs) {
        out += "<|im_start|>";
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "system";    break;
            case MessageRole::USER:      out += "user";      break;
            case MessageRole::ASSISTANT: out += "assistant"; break;
            case MessageRole::TOOL:      out += "tool";      break;
        }
        out += '\n';
        out += msg.content;
        out += "<|im_end|>\n";
    }
    bool lastIsAssistant = !msgs.empty() && msgs.back().role == MessageRole::ASSISTANT;
    if (!lastIsAssistant) {
        out += "<|im_start|>assistant\n";
    }
    return out;
}

//  Llama 3

std::string Llama3PromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string out = "<|begin_of_text|>";
    for (const auto& msg : msgs) {
        out += "<|start_header_id|>";
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "system";    break;
            case MessageRole::USER:      out += "user";      break;
            case MessageRole::ASSISTANT: out += "assistant"; break;
            case MessageRole::TOOL:      out += "tool";      break;
        }
        out += "<|end_header_id|>\n\n";
        out += msg.content;
        out += "<|eot_id|>";
    }
    bool lastIsAssistant = !msgs.empty() && msgs.back().role == MessageRole::ASSISTANT;
    if (!lastIsAssistant) {
        out += "<|start_header_id|>assistant<|end_header_id|>\n\n";
    }
    return out;
}

//  Llama 2

std::string Llama2PromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string systemContent;
    std::vector<std::string> userTurns;
    std::vector<std::string> assistantTurns;
    bucketMessagesForInstFormat(msgs, systemContent, userTurns, assistantTurns);

    if (userTurns.empty()) {
        spdlog::warn("[PromptTemplate] LLAMA2 format: no user messages found");
        return "";
    }

    std::string out;
    for (size_t i = 0; i < userTurns.size(); ++i) {
        out += "[INST] ";
        if (i == 0 && !systemContent.empty()) {
            out += "<<SYS>>\n";
            out += systemContent;
            out += "\n<</SYS>>\n\n";
        }
        out += userTurns[i];
        out += " [/INST]";
        if (i < assistantTurns.size()) {
            out += ' ';
            out += assistantTurns[i];
        }
    }
    return out;
}

//  Mistral

std::string MistralPromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string systemContent;
    std::vector<std::string> userTurns;
    std::vector<std::string> assistantTurns;
    bucketMessagesForInstFormat(msgs, systemContent, userTurns, assistantTurns);

    if (userTurns.empty()) {
        spdlog::warn("[PromptTemplate] MISTRAL format: no user messages found");
        return "";
    }

    std::string out;
    for (size_t i = 0; i < userTurns.size(); ++i) {
        out += "[INST] ";
        std::string userText = userTurns[i];
        if (i == 0 && !systemContent.empty()) {
            userText = systemContent + "\n\n" + userText;
        }
        out += userText;
        out += " [/INST]";
        if (i < assistantTurns.size()) {
            out += ' ';
            out += assistantTurns[i];
        }
    }
    return out;
}

//  Gemma

std::string GemmaPromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string out;
    for (const auto& msg : msgs) {
        out += "<start_of_turn>";
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "user";  break; // Gemma has no system role; prepend to user turn
            case MessageRole::USER:      out += "user";  break;
            case MessageRole::ASSISTANT: out += "model"; break;
            case MessageRole::TOOL:      out += "user";  break; // Gemma has no tool role; surface as user turn
        }
        out += '\n';
        out += msg.content;
        out += "<end_of_turn>\n";
    }
    bool lastIsAssistant = !msgs.empty() && msgs.back().role == MessageRole::ASSISTANT;
    if (!lastIsAssistant) {
        out += "<start_of_turn>model\n";
    }
    return out;
}

//  Phi-3

std::string Phi3PromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string out;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& msg = msgs[i];
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "<|system|>\n";    break;
            case MessageRole::USER:      out += "<|user|>\n";      break;
            case MessageRole::ASSISTANT: out += "<|assistant|>\n"; break;
            case MessageRole::TOOL:      out += "<|user|>\n";      break; // Phi3 has no tool role; surface as user turn
        }
        out += msg.content;
        // No closing <|end|> after the last assistant message - model generates from here
        bool isLastMsg = (i == msgs.size() - 1);
        bool isAssistant = (msg.role == MessageRole::ASSISTANT);
        if (!(isLastMsg && isAssistant)) {
            out += "<|end|>\n";
        }
    }
    out += "<|assistant|>\n";
    return out;
}

//  LFM2.5

std::string LFM2_5PromptTemplate::render() const {
    auto msgs = resolveAll();
    std::string out = "<|startoftext|>";
    for (const auto& msg : msgs) {
        out += "<|im_start|>";
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "system";    break;
            case MessageRole::USER:      out += "user";      break;
            case MessageRole::ASSISTANT: out += "assistant"; break;
            case MessageRole::TOOL:      out += "tool";      break;
        }
        out += '\n';
        out += msg.content;
        out += "<|im_end|>\n";
    }
    bool lastIsAssistant = !msgs.empty() && msgs.back().role == MessageRole::ASSISTANT;
    if (!lastIsAssistant) {
        out += "<|im_start|>assistant\n";
    }
    return out;
}

std::string LFM2_5PromptTemplate::renderToolPrompt(
        const std::vector<const Tool*>& enabledTools,
        const std::string& /*textFieldGuidance - no "text" field in the native format*/) const {
    if (enabledTools.empty()) return {};

    // Replicate the format the model's Jinja chat template generates natively when
    // tools are passed via the llama.cpp tools interface:
    //   "List of tools: [{...tool1...}, {...tool2...}]"
    // We bypass the native interface (tools are in the system prompt), so we
    // reproduce the exact string the template would emit, then add call instructions.
    std::string out = "List of tools: [";
    bool first = true;
    for (const Tool* tool : enabledTools) {
        if (!first) out += ", ";
        out += tool->toJson().dump();
        first = false;
    }
    out += "]\n\n";
    out += lfmCallInstruction(toolCallTags());
    out += "\n\n"
           "For non-tool replies, respond with valid JSON and nothing else:\n"
           "{\"text\": \"Your spoken reply here.\", \"isFinal\": true}\n"
           "Add \"isExpecting\": true when asking the user a question. "
           "Add \"choices\": [{\"text\": \"Option A\"}, {\"text\": \"Option B\"}] when the user must pick from a fixed set. "
           "Never add keys outside this schema or wrap the JSON in markdown.";
    return out;
}

//  LFM2

std::string LFM2PromptTemplate::render() const {
    // LFM2 differs from LFM2_5 only in that tool-result messages are additionally wrapped
    // in <|tool_response_start|>…<|tool_response_end|> inside the message content block.
    auto msgs = resolveAll();
    std::string out = "<|startoftext|>";
    for (const auto& msg : msgs) {
        out += "<|im_start|>";
        switch (msg.role) {
            case MessageRole::SYSTEM:    out += "system";    break;
            case MessageRole::USER:      out += "user";      break;
            case MessageRole::ASSISTANT: out += "assistant"; break;
            case MessageRole::TOOL:      out += "tool";      break;
        }
        out += '\n';
        if (msg.role == MessageRole::TOOL) {
            out += "<|tool_response_start|>";
            out += msg.content;
            out += "<|tool_response_end|>";
        } else {
            out += msg.content;
        }
        out += "<|im_end|>\n";
    }
    bool lastIsAssistant = !msgs.empty() && msgs.back().role == MessageRole::ASSISTANT;
    if (!lastIsAssistant) {
        out += "<|im_start|>assistant\n";
    }
    return out;
}

std::string LFM2PromptTemplate::renderToolPrompt(
        const std::vector<const Tool*>& enabledTools,
        const std::string& /*textFieldGuidance - no "text" field in the native format*/) const {
    if (enabledTools.empty()) return {};

    nlohmann::json toolArray = nlohmann::json::array();
    for (const Tool* tool : enabledTools) {
        toolArray.push_back(tool->toJson());
    }

    // Tool list wrapped in the format's native special tokens, plus call instructions.
    std::string out = "<|tool_list_start|>\n";
    out += toolArray.dump(2);
    out += "\n<|tool_list_end|>\n\n";
    out += lfmCallInstruction(toolCallTags());
    out += "\n\n"
           "For non-tool replies, respond with valid JSON and nothing else:\n"
           "{\"text\": \"Your spoken reply here.\", \"isFinal\": true}\n"
           "Add \"isExpecting\": true when asking the user a question. "
           "Add \"choices\": [{\"text\": \"Option A\"}, {\"text\": \"Option B\"}] when the user must pick from a fixed set. "
           "Never add keys outside this schema or wrap the JSON in markdown.";
    return out;
}

} // namespace llm
} // namespace va
