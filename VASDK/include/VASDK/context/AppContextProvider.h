#ifndef APPCONTEXTPROVIDER_H
#define APPCONTEXTPROVIDER_H

#include "ContextProvider.h"
#include <optional>
#include <unordered_map>

namespace va {
    namespace context {
        /**
         * @brief Manages per-app context (id, name, version, build number).
         */
        class AppContextProvider : public ContextProvider {
            public:
            AppContextProvider() = default;
            AppContextProvider(const AppContextProvider&) = delete;
            AppContextProvider& operator=(const AppContextProvider&) = delete;
            ~AppContextProvider() override = default;

            struct AppInfo {
                std::string id;
                std::string name;
                std::string version;
                std::string buildNumber;
                std::optional<std::string> installerPackage; 
            };

            virtual void setAppInfo(std::string id, const AppInfo& info);
            virtual const AppInfo& getAppInfo(const std::string& id) const;

            private:
            std::unordered_map<std::string, AppInfo> _appInfo;
        };
    }
}

#endif
