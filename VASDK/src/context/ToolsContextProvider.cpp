#include "VASDK/context/ToolsContextProvider.h"

namespace va {
    namespace context {
        void ToolsContextProvider::setTools(const std::unordered_map<std::string, Tool>& tools) {
            _tools = tools;
            syncToContext();
        }

        void ToolsContextProvider::addTool(const Tool& tool) {
            _tools[tool.name] = tool;
            syncToContext();
        }

        void ToolsContextProvider::removeTool(const std::string& name) {
            _tools.erase(name);
            syncToContext();
        }

        const std::unordered_map<std::string, ToolsContextProvider::Tool>& ToolsContextProvider::getTools() const {
            return _tools;
        }

        void ToolsContextProvider::syncToContext() {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& [name, t] : _tools) {
                nlohmann::json entry;
                entry["name"]        = t.name;
                entry["description"] = t.description;
                entry["enabled"]     = t.enabled;
                entry["parameters"]  = t.parameters;
                arr.push_back(entry);
            }
            setValue("tools", arr);
        }
    }
}
