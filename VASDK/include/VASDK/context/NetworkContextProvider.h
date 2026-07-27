#ifndef NETWORKCONTEXTPROVIDER_H
#define NETWORKCONTEXTPROVIDER_H

#include "ContextProvider.h"
#include <optional>

namespace va {
    namespace context {
        /**
         * @brief Manages network-related context (Wi-Fi, cellular, connection quality).
         */
        class NetworkContextProvider : public ContextProvider {
            public:
            NetworkContextProvider() = default;
            NetworkContextProvider(const NetworkContextProvider&) = delete;
            NetworkContextProvider& operator=(const NetworkContextProvider&) = delete;
            ~NetworkContextProvider() override = default;

            enum NetworkTypeFlags {
                None = 0,
                WIFI = 1,
                CELLULAR = 2,
                ETHERNET = 3,
                UNKNOWN = 4
            };

            enum ConnectionQuality {
                EXCELLENT = 0,
                GOOD = 1,
                FAIR = 2,
                POOR = 3,
                UNKNOWN_QUALITY = 4
            };

            struct WifiInfo {
                std::string ssid;
                std::string bssid;
                ConnectionQuality quality;
            };

            struct CellularInfo {
                std::string carrier;
                int signalStrength;
                ConnectionQuality quality;
            };

            struct NetworkInfo {
                int flag = 0;
                std::optional<WifiInfo> wifi = std::nullopt;
                std::optional<CellularInfo> cellular = std::nullopt;
            };

            virtual void setNetworkInfo(const NetworkInfo& info);
            virtual const NetworkInfo& getNetworkInfo() const;

            private:
            NetworkInfo _networkInfo;
        };
    }
}

#endif