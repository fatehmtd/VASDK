#include <VASDK/llm/Tool.h>
#include <spdlog/spdlog.h>

namespace va {
namespace llm {

Tool::Tool(std::string name, std::string description)
    : _name(std::move(name)), _description(std::move(description)) {}

Tool& Tool::addParameter(Parameter param) {
    _parameters.push_back(std::move(param));
    return *this;
}

Tool& Tool::onCall(Handler handler) {
    _handler = std::move(handler);
    return *this;
}

Tool& Tool::setEnabled(bool enabled) {
    _enabled = enabled;
    return *this;
}

const std::string&            Tool::name()        const { return _name; }
const std::string&            Tool::description() const { return _description; }
const std::vector<Tool::Parameter>& Tool::parameters()  const { return _parameters; }
bool                          Tool::isEnabled()   const { return _enabled; }

nlohmann::json Tool::execute(const nlohmann::json& args) const {
    if (!_handler) {
        spdlog::error("[Tool] No handler registered for '{}'", _name);
        return {{"error", "no handler registered for tool: " + _name}};
    }
    try {
        return _handler(args);
    } catch (const std::exception& e) {
        spdlog::error("[Tool] Handler for '{}' threw: {}", _name, e.what());
        return {{"error", e.what()}};
    }
}

nlohmann::json Tool::toJson() const {
    nlohmann::json props   = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();

    for (const auto& p : _parameters) {
        props[p.name] = {{"type", p.type}, {"description", p.description}};
        if (p.required) required.push_back(p.name);
    }

    return {
        {"name", _name},
        {"description", _description},
        {"parameters", {
            {"type", "object"},
            {"properties", props},
            {"required", required}
        }}
    };
}

std::string Tool::toPromptDescription() const {
    std::string result = _name + "(";
    for (size_t i = 0; i < _parameters.size(); ++i) {
        const auto& p = _parameters[i];
        if (i > 0) result += ", ";
        result += p.name + (p.required ? "*" : "?") + ": " + p.type;
        if (!p.description.empty()) result += " - " + p.description;
    }
    result += "): " + _description;
    return result;
}

} // namespace llm
} // namespace va
