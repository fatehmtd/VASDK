#ifndef TOOLSCONTEXTPROVIDER_H
#define TOOLSCONTEXTPROVIDER_H

#include "ContextProvider.h"
#include <unordered_map>

namespace va {
    namespace context {
        /**
         * @brief Manages tool-related context (available tools and their enabled state).
         */
        class ToolsContextProvider : public ContextProvider {
            public:
            ToolsContextProvider() = default;
            ToolsContextProvider(const ToolsContextProvider&) = delete;
            ToolsContextProvider& operator=(const ToolsContextProvider&) = delete;
            ~ToolsContextProvider() override = default;

            struct Tool {
                std::string name;
                std::string description;
                std::unordered_map<std::string, std::string> parameters;
                bool        enabled;
            };

            virtual void setTools(const std::unordered_map<std::string, Tool>& tools);
            // Inserts a new tool or replaces an existing one with the same name.
            virtual void addTool(const Tool& tool);
            virtual void removeTool(const std::string& name);
            virtual const std::unordered_map<std::string, Tool>& getTools() const;

            private:
            std::unordered_map<std::string, Tool> _tools;

            void syncToContext();
        };
    }
}

#endif
