#include <VASDK/llm/PromptTemplate.h>
#include <VASDK/llm/ApiPromptTemplates.h>
#include <VASDK/llm/LocalPromptTemplates.h>
#include <VASDK/llm/Tool.h>
#include <spdlog/spdlog.h>

namespace va {
namespace llm {

//  Builder

PromptTemplate& PromptTemplate::addSystem(const std::string& tmplContent) {
    return addMessage(MessageRole::SYSTEM, tmplContent);
}

PromptTemplate& PromptTemplate::addUser(const std::string& tmplContent) {
    return addMessage(MessageRole::USER, tmplContent);
}

PromptTemplate& PromptTemplate::addAssistant(const std::string& tmplContent) {
    return addMessage(MessageRole::ASSISTANT, tmplContent);
}

PromptTemplate& PromptTemplate::addMessage(MessageRole role, const std::string& tmplContent) {
    _messages.push_back(TemplateMessage{role, tmplContent});
    return *this;
}

//  Variable substitution

PromptTemplate& PromptTemplate::set(const std::string& varName, const std::string& value) {
    _variables[varName] = value;
    return *this;
}

PromptTemplate& PromptTemplate::setAll(const std::unordered_map<std::string, std::string>& vars) {
    for (const auto& [k, v] : vars) {
        _variables[k] = v;
    }
    return *this;
}

PromptTemplate& PromptTemplate::unset(const std::string& varName) {
    _variables.erase(varName);
    return *this;
}

//  Validation

std::vector<std::string> PromptTemplate::missingVariables() const {
    std::vector<std::string> missing;
    for (const auto& msg : _messages) {
        const std::string& tmpl = msg._templateContent;
        size_t i = 0;
        while (i < tmpl.size()) {
            if (tmpl[i] == '{') {
                if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
                    i += 2;
                    continue;
                }
                size_t end = tmpl.find('}', i + 1);
                if (end != std::string::npos) {
                    std::string name = tmpl.substr(i + 1, end - i - 1);
                    bool valid = !name.empty();
                    for (char c : name) {
                        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                            valid = false;
                            break;
                        }
                    }
                    if (valid && _variables.find(name) == _variables.end()) {
                        bool already = false;
                        for (const auto& m : missing) {
                            if (m == name) { already = true; break; }
                        }
                        if (!already) missing.push_back(name);
                    }
                    i = end + 1;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        }
    }
    return missing;
}

std::vector<std::string> PromptTemplate::unusedVariables() const {
    std::vector<std::string> unused;
    for (const auto& [name, _] : _variables) {
        bool found = false;
        for (const auto& msg : _messages) {
            if (msg._templateContent.find('{' + name + '}') != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) unused.push_back(name);
    }
    return unused;
}

void PromptTemplate::setMissingVarPolicy(MissingVarPolicy policy) {
    _missingVarPolicy = policy;
}

MissingVarPolicy PromptTemplate::getMissingVarPolicy() const {
    return _missingVarPolicy;
}

void PromptTemplate::clear() {
    _messages.clear();
    _variables.clear();
}

//  Core helpers

std::string PromptTemplate::substituteVars(const std::string& tmpl) const {
    std::string result;
    result.reserve(tmpl.size());

    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
                result += '{';
                i += 2;
                continue;
            }
            size_t end = tmpl.find('}', i + 1);
            if (end != std::string::npos) {
                std::string name = tmpl.substr(i + 1, end - i - 1);
                bool valid = !name.empty();
                for (char c : name) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    auto it = _variables.find(name);
                    if (it != _variables.end()) {
                        result += it->second;
                    } else {
                        switch (_missingVarPolicy) {
                            case MissingVarPolicy::THROW:
                                throw PromptTemplateError("Missing template variable: {" + name + "}");
                            case MissingVarPolicy::LEAVE_PLACEHOLDER:
                                result += '{';
                                result += name;
                                result += '}';
                                break;
                            case MissingVarPolicy::EMPTY_STRING:
                                break;
                        }
                    }
                    i = end + 1;
                } else {
                    result += tmpl[i];
                    ++i;
                }
            } else {
                result += tmpl[i];
                ++i;
            }
        } else if (tmpl[i] == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
            result += '}';
            i += 2;
        } else {
            result += tmpl[i];
            ++i;
        }
    }

    return result;
}

std::vector<ConversationMessage> PromptTemplate::resolveAll() const {
    if (_messages.empty()) {
        spdlog::warn("[PromptTemplate] render() called on empty template");
    }
    std::vector<ConversationMessage> resolved;
    resolved.reserve(_messages.size());
    for (const auto& msg : _messages) {
        ConversationMessage cm;
        cm.role = msg._role;
        cm.content = substituteVars(msg._templateContent);
        cm.timestamp = 0;
        resolved.push_back(std::move(cm));
    }
    return resolved;
}

//  Integration

std::vector<ConversationMessage> PromptTemplate::getMessages() const {
    return resolveAll();
}

//  Tool prompt
std::string PromptTemplate::renderToolPrompt(
        const std::vector<const Tool*>& enabledTools,
        const std::string& textFieldGuidance) const {
    if (enabledTools.empty()) return {};

    std::string toolList;
    for (const Tool* tool : enabledTools) {
        toolList += tool->toPromptDescription();
        toolList += "\n";
    }

    return
        std::string("RESPONSE FORMAT - you MUST always reply with a single JSON object and nothing else. "
        "Reply with exactly ONE of these two shapes, never a mix of both:\n"
        "Shape 1 - normal reply, no tool needed:\n"
        "{\"text\": \"<") + textFieldGuidance + ">\"}\n"
        "The text between < and > is a DESCRIPTION of what to put there, not literal text - "
        "replace the whole placeholder, brackets included, with your own words. "
        "Never copy the placeholder description into your reply.\n"
        "Shape 2 - the request requires a tool from the list below:\n"
        "{\"tools\": [{\"tool\": \"tool_name\", \"args\": {}}]}\n"
        "Fill \"args\" with the parameters the tool lists below; use {} when it takes none. "
        "You MAY include multiple tools in the array - they will run in parallel.\n"
        "Shape 1 may also carry \"isExpecting\": true when your reply asks the user a question, "
        "and \"choices\": [{\"text\": \"Small\"}, {\"text\": \"Large\"}] when the user must pick "
        "from a fixed set of options - replace the example values with the real options.\n"
        "Never output anything outside the JSON object. No markdown, no prose, no code blocks.\n"
        "Only use \"tool\" names that appear verbatim in the \"Available tools\" list below - "
        "never invent a tool name, even if one would be convenient. "
        "If a listed tool can fulfill the request, you MUST reply with Shape 2 and call it - "
        "never answer from guesswork or memory, and never fabricate a detail (a phone number, "
        "a device's on/off state, whether something exists) that a tool would actually tell you. "
        "Only reply with Shape 1 when no listed tool applies, or the user is just chatting or "
        "greeting you.\n"
        "After tool results are provided, reply with your final answer as Shape 1 - "
        "never repeat a tool call whose result you already have.\n\n"
        "Available tools ('*' = required param, '?' = optional):\n" + toolList;
}

//  Static helpers

std::optional<ChatFormat> PromptTemplate::defaultFormatForProvider(LLMProvider provider) {
    switch (provider) {
        case LLMProvider::OPENAI:     return ChatFormat::OPENAI;
        case LLMProvider::ANTHROPIC:  return ChatFormat::ANTHROPIC;
        case LLMProvider::OPENROUTER: return ChatFormat::OPENAI;
        case LLMProvider::CUSTOM:     return std::nullopt;
        case LLMProvider::LLAMA_CPP:  return std::nullopt;
        case LLMProvider::LFM:        return std::nullopt; // GGUF carries its own embedded template
    }
    return std::nullopt;
}

std::unique_ptr<PromptTemplate> PromptTemplate::create(ChatFormat format) {
    switch (format) {
        case ChatFormat::OPENAI:    return std::make_unique<OpenAIPromptTemplate>();
        case ChatFormat::ANTHROPIC: return std::make_unique<AnthropicPromptTemplate>();
        case ChatFormat::GOOGLE:    return std::make_unique<GooglePromptTemplate>();
        case ChatFormat::CHATML:    return std::make_unique<ChatMLPromptTemplate>();
        case ChatFormat::LLAMA3:    return std::make_unique<Llama3PromptTemplate>();
        case ChatFormat::LLAMA2:    return std::make_unique<Llama2PromptTemplate>();
        case ChatFormat::MISTRAL:   return std::make_unique<MistralPromptTemplate>();
        case ChatFormat::GEMMA:     return std::make_unique<GemmaPromptTemplate>();
        case ChatFormat::PHI3:      return std::make_unique<Phi3PromptTemplate>();
        case ChatFormat::LFM2:      return std::make_unique<LFM2PromptTemplate>();
        case ChatFormat::LFM2_5:    return std::make_unique<LFM2_5PromptTemplate>();
    }
    return nullptr;
}

} // namespace llm
} // namespace va
