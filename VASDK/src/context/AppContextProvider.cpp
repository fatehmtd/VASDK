#include "VASDK/context/AppContextProvider.h"

namespace va {
    namespace context {
        void AppContextProvider::setAppInfo(std::string id, const AppInfo& info) {
            _appInfo[id] = info;
            nlohmann::json apps = nlohmann::json::object();
            for (const auto& [key, entry] : _appInfo) {
                nlohmann::json app;
                app["id"]           = entry.id;
                app["name"]         = entry.name;
                app["version"]      = entry.version;
                app["build_number"] = entry.buildNumber;
                if (entry.installerPackage) {
                    app["installer_package"] = entry.installerPackage.value();
                }
                apps[key] = app;
            }
            setValue("apps", apps);
        }

        const AppContextProvider::AppInfo& AppContextProvider::getAppInfo(const std::string& id) const {
            return _appInfo.at(id);
        }
    }
}
