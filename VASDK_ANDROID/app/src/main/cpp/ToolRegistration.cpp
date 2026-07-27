#include "ToolRegistration.h"

#include "tools/android_tools.h"

void registerSystemTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
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

void registerMediaTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
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

void registerNotificationAndAlarmTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Notification & alarm tools (notification_tools.h)
    session->toolDispatcher->registerTool(SendNotificationTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetAlarmOrTimerTool{callKotlinTool});
}

void registerCommunicationTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Communication tools (communication_tools.h)
    session->toolDispatcher->registerTool(GetContactsTool{callKotlinTool});
    session->toolDispatcher->registerTool(GetCallLogsTool{callKotlinTool});
    session->toolDispatcher->registerTool(MakePhoneCallTool{callKotlinTool});
    session->toolDispatcher->registerTool(SendSmsTool{callKotlinTool});
}

void registerCalendarTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    session->toolDispatcher->registerTool(GetCalendarEventsTool{callKotlinTool});
    session->toolDispatcher->registerTool(AddCalendarEventTool{callKotlinTool});
}

void registerLocationTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Location tools (location_tools.h)
    session->toolDispatcher->registerTool(GetLocationTool{callKotlinTool});
    session->toolDispatcher->registerTool(NavigateToTool{callKotlinTool});
}

void registerWebTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Web tools (web_tools.h)
    session->toolDispatcher->registerTool(WebSearchTool{callKotlinTool});
    session->toolDispatcher->registerTool(FetchUrlTool{callKotlinTool});
}

void registerCameraTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Camera tools (camera_tools.h)
    session->toolDispatcher->registerTool(CapturePhotoTool{callKotlinTool});
    session->toolDispatcher->registerTool(SetFlashlightTool{callKotlinTool});
}

void registerAndroidTools(VasdkSession *session, AndroidToolBridge callKotlinTool) {
    // Pure C++ tools - no JNI round-trip needed
    session->toolDispatcher->registerTool(DateTimeTool{});
    session->toolDispatcher->registerTool(RandomNumberTool{});
    session->toolDispatcher->registerTool(UnitConvertTool{});
}
