#pragma once
#include "bridge_tool.h"

//  get_battery_status 
class BatteryStatusTool : public BridgeTool {
public:
    explicit BatteryStatusTool(AndroidToolBridge bridge)
        : BridgeTool("get_battery_status",
                     "Get the battery level percentage and charging state",
                     std::move(bridge)) {}
};

//  get_device_info 
class DeviceInfoTool : public BridgeTool {
public:
    explicit DeviceInfoTool(AndroidToolBridge bridge)
        : BridgeTool("get_device_info",
                     "Get the device model, manufacturer, and Android version",
                     std::move(bridge)) {}
};

//  get_wifi_info 
class GetWifiInfoTool : public BridgeTool {
public:
    explicit GetWifiInfoTool(AndroidToolBridge bridge)
        : BridgeTool("get_wifi_info",
                     "Get the WiFi status, network name, and signal strength",
                     std::move(bridge)) {}
};

//  get_network_status 
class GetNetworkStatusTool : public BridgeTool {
public:
    explicit GetNetworkStatusTool(AndroidToolBridge bridge)
        : BridgeTool("get_network_status",
                     "Get whether the device is online and the connection type (WiFi, cellular)",
                     std::move(bridge)) {}
};

//  get_storage_info 
class GetStorageInfoTool : public BridgeTool {
public:
    explicit GetStorageInfoTool(AndroidToolBridge bridge)
        : BridgeTool("get_storage_info",
                     "Get total and free internal storage in gigabytes",
                     std::move(bridge)) {}
};

//  get_screen_brightness 
class GetScreenBrightnessTool : public BridgeTool {
public:
    explicit GetScreenBrightnessTool(AndroidToolBridge bridge)
        : BridgeTool("get_screen_brightness",
                     "Get the current screen brightness (0-255). Takes no arguments - no level needed",
                     std::move(bridge)) {}
};

//  set_screen_brightness 
class SetScreenBrightnessTool : public BridgeTool {
public:
    explicit SetScreenBrightnessTool(AndroidToolBridge bridge)
        : BridgeTool("set_screen_brightness",
                     "Set the screen brightness. Only when the user explicitly asks to change brightness",
                     std::move(bridge)) {
        addParameter({"level", "Brightness level (integer, 0 = minimum, 255 = maximum)", "integer", true});
    }
};

//  set_ringer_mode 
class SetRingerModeTool : public BridgeTool {
public:
    explicit SetRingerModeTool(AndroidToolBridge bridge)
        : BridgeTool("set_ringer_mode",
                     "Set the ringer mode. Only when the user explicitly asks to silence, vibrate, or unmute the phone",
                     std::move(bridge)) {
        addParameter({"mode", "Ringer mode: 'normal', 'vibrate', or 'silent'", "string", true});
    }
};

//  get_installed_apps 
class GetInstalledAppsTool : public BridgeTool {
public:
    explicit GetInstalledAppsTool(AndroidToolBridge bridge)
        : BridgeTool("get_installed_apps",
                     "List the names of user-installed apps",
                     std::move(bridge)) {}
};
