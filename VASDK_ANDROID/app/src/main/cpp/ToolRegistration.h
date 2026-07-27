#pragma once

#include "VasdkSession.h"
#include "tools/bridge_tool.h"

// Registers each android_tools.h tool group with session->toolDispatcher, wired to
// call back into Kotlin via callKotlinTool. Split into groups so nativeInit can
// enable/disable them independently (see the HINT comment at the call site: the
// more tools registered, the less reliably small on-device models call any one
// of them).
void registerSystemTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerMediaTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerNotificationAndAlarmTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerCommunicationTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerCalendarTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerLocationTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerWebTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerCameraTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
void registerAndroidTools(VasdkSession *session, AndroidToolBridge callKotlinTool);
