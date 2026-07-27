#include "VASDK/context/NetworkContextProvider.h"

namespace va {
    namespace context {
        void NetworkContextProvider::setNetworkInfo(const NetworkInfo& info) {
            _networkInfo = info;
            setValue("network_flag", info.flag);
            if (info.wifi) {
                nlohmann::json wifiJson;
                wifiJson["ssid"]  = info.wifi->ssid;
                wifiJson["bssid"] = info.wifi->bssid;
                setValue("wifi_info", wifiJson);
            }
            if (info.cellular) {
                nlohmann::json cellularJson;
                cellularJson["carrier"]         = info.cellular->carrier;
                cellularJson["signal_strength"] = info.cellular->signalStrength;
                setValue("cellular_info", cellularJson);
            }
        }

        const NetworkContextProvider::NetworkInfo& NetworkContextProvider::getNetworkInfo() const {
            return _networkInfo;
        }
    }
}
