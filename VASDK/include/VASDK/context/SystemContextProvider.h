#ifndef SYSTEMCONTEXTPROVIDER_H
#define SYSTEMCONTEXTPROVIDER_H

#include "ContextProvider.h"
#include <cstdint>
#include <optional>

namespace va {
    namespace context {
        /**
         * @brief Manages system-related context (device, battery, memory, CPU).
         */
        class SystemContextProvider : public ContextProvider {
            public:
            SystemContextProvider() = default;
            SystemContextProvider(const SystemContextProvider&) = delete;
            SystemContextProvider& operator=(const SystemContextProvider&) = delete;
            ~SystemContextProvider() override = default;

            struct DeviceInfo {
                std::string model;
                std::string manufacturer;
                std::string osName;
                std::string osVersion;
            };

            struct BatteryInfo {
                int  level;      // 0–100
                bool isCharging;
            };

            struct MemoryInfo {
                uint64_t totalMB;
                uint64_t availableMB;
            };

            struct CpuInfo {
                std::string architecture;
                int coreCount;
                std::vector<int> usagePerCore;
            };

            struct SystemInfo {
                std::optional<DeviceInfo>  device{};
                std::optional<BatteryInfo> battery{};
                std::optional<MemoryInfo>  memory{};
                std::optional<CpuInfo>     cpu{};
            };

            virtual void setSystemInfo(const SystemInfo& info);
            virtual const SystemInfo& getSystemInfo() const;

            private:
            SystemInfo _systemInfo;
        };
    }
}

#endif
