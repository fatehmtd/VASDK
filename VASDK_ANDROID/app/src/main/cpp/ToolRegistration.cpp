#include "ToolRegistration.h"

#include "tools/android_tools.h"

void ToolRegistry::registerSystemTools() {
    // System tools (system_tools.h)
    session->toolDispatcher->registerTool(BatteryStatusTool{callKotlinTool});
    session->toolDispatcher->registerTool(DeviceInfoTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetWifiInfoTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetNetworkStatusTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetStorageInfoTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetScreenBrightnessTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetScreenBrightnessTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetRingerModeTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetInstalledAppsTool{callKotlinTool});
}

void ToolRegistry::registerMediaTools() {
    // Media tools (media_tools.h)
    session->toolDispatcher->registerTool(GetVolumeTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetVolumeTool{callKotlinTool});
    session->toolDispatcher->registerTool(MediaPlayTool{callKotlinTool});
    session->toolDispatcher->registerTool(MediaPauseTool{callKotlinTool});
    session->toolDispatcher->registerTool(MediaPlayPauseTool{callKotlinTool});
    session->toolDispatcher->registerTool(MediaNextTrackTool{callKotlinTool});
    session->toolDispatcher->registerTool(MediaPrevTrackTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetNowPlayingTool{callKotlinTool});
}

void ToolRegistry::registerNotificationAndAlarmTools() {
    // Notification & alarm tools (notification_tools.h)
    session->toolDispatcher->registerTool(SendNotificationTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetAlarmOrTimerTool{callKotlinTool});
}

void ToolRegistry::registerCommunicationTools() {
    // Communication tools (communication_tools.h)
    session->toolDispatcher->registerTool(GetContactsTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetCallLogsTool{callKotlinTool});
    session->toolDispatcher->registerTool(MakePhoneCallTool{callKotlinTool});
    session->toolDispatcher->registerTool(SendSmsTool{callKotlinTool});
}

void ToolRegistry::registerCalendarTools() {
    session->toolDispatcher->registerTool(GetCalendarEventsTool{callKotlinTool});
    session->toolDispatcher->registerTool(AddCalendarEventTool{callKotlinTool});
}

void ToolRegistry::registerLocationTools() {
    // Location tools (location_tools.h)
    session->toolDispatcher->registerTool(GetLocationTool{callKotlinTool});
    session->toolDispatcher->registerTool(NavigateToTool{callKotlinTool});
}

void ToolRegistry::registerWebTools() {
    // Web tools (web_tools.h)
    session->toolDispatcher->registerTool(WebSearchTool{callKotlinTool});
    session->toolDispatcher->registerTool(FetchUrlTool{callKotlinTool});
}

void ToolRegistry::registerCameraTools() {
    // Camera tools (camera_tools.h)
    session->toolDispatcher->registerTool(CapturePhotoTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetFlashlightTool{callKotlinTool});
}

void ToolRegistry::registerAndroidTools() {
    // Pure C++ tools - no JNI round-trip needed
    session->toolDispatcher->registerTool(DateTimeTool{});
    session->toolDispatcher->registerTool(RandomNumberTool{});
    session->toolDispatcher->registerTool(UnitConvertTool{});
}
