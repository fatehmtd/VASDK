#pragma once

#include "vasdk_export.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace va {
namespace llm {

class VASDK_EXPORT Tool {
public:
    struct Parameter {
        std::string name;
        std::string description;
        std::string type     = "string";
        bool        required = true;
    };

    using Handler = std::function<nlohmann::json(const nlohmann::json& args)>;

    Tool() = default;
    Tool(std::string name, std::string description);

    Tool& addParameter(Parameter param);
    Tool& onCall(Handler handler);
    Tool& setEnabled(bool enabled);

    const std::string&            name()        const;
    const std::string&            description() const;
    const std::vector<Parameter>& parameters()  const;
    bool                          isEnabled()   const;

    nlohmann::json execute(const nlohmann::json& args) const;

    nlohmann::json toJson()              const;
    std::string    toPromptDescription() const;

private:
    std::string            _name;
    std::string            _description;
    std::vector<Parameter> _parameters;
    Handler                _handler;
    bool                   _enabled = true;
};

} // namespace llm
} // namespace va
