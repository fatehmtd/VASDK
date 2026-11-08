#pragma once

#include "VasdkSession.h"
#include "tools/bridge_tool.h"

// Registers each android_tools.h tool group with session->toolDispatcher, wired to
// call back into Kotlin via callKotlinTool. Split into groups so nativeInit can
// enable/disable them independently (see the HINT comment at the call site: the
// more tools registered, the less reliably small on-device models call any one
// of them).

/**
 * Tool registration.
 */
class ToolRegistry {
    public:
    ToolRegistry(VasdkSession *session, AndroidToolBridge callKotlinTool): session(session), callKotlinTool(callKotlinTool) {}

    void registerSystemTools();
    void registerMediaTools();
    void registerNotificationAndAlarmTools();
    void registerCommunicationTools();
    void registerCalendarTools();
    void registerLocationTools();
    void registerWebTools();
    void registerCameraTools();
    void registerAndroidTools();
private:
    VasdkSession *session;
    AndroidToolBridge callKotlinTool;
};