#include "VASDK/context/SystemContextProvider.h"

namespace va {
    namespace context {
        void SystemContextProvider::setSystemInfo(const SystemInfo& info) {
            _systemInfo = info;
            if (info.device) {
                nlohmann::json device;
                device["model"]        = info.device->model;
                device["manufacturer"] = info.device->manufacturer;
                device["os_name"]      = info.device->osName;
                device["os_version"]   = info.device->osVersion;
                setValue("device", device);
            }
            if (info.battery) {
                nlohmann::json battery;
                battery["level"]       = info.battery->level;
                battery["is_charging"] = info.battery->isCharging;
                setValue("battery", battery);
            }
            if (info.memory) {
                nlohmann::json memory;
                memory["total_mb"]     = info.memory->totalMB;
                memory["available_mb"] = info.memory->availableMB;
                setValue("memory", memory);
            }
            if (info.cpu) {
                nlohmann::json cpu;
                cpu["architecture"]  = info.cpu->architecture;
                cpu["core_count"]    = info.cpu->coreCount;
                cpu["usage_per_core"] = info.cpu->usagePerCore;
                setValue("cpu", cpu);
            }
        }

        const SystemContextProvider::SystemInfo& SystemContextProvider::getSystemInfo() const {
            return _systemInfo;
        }
    }
}
