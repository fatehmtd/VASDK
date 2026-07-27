#pragma once
#include "bridge_tool.h"

//  set_flashlight
class SetFlashlightTool : public BridgeTool {
public:
    explicit SetFlashlightTool(AndroidToolBridge bridge)
            : BridgeTool("set_flashlight",
                         "Turn the flashlight (torch) on or off. This tool can do both: "
                         "call it with on=true to turn the light on, call it with on=false to turn the light off. "
                         "Only use it when the user explicitly asks for the flashlight or torch",
                         std::move(bridge)) {
        addParameter({"on", "true turns the flashlight on, false turns it off", "boolean", true});
    }
};

//  capture_photo 
class CapturePhotoTool : public BridgeTool {
public:
    explicit CapturePhotoTool(AndroidToolBridge bridge)
        : BridgeTool("capture_photo",
                     "Take a photo with the rear camera. Only when the user explicitly asks to take a photo. Takes no arguments",
                     std::move(bridge)) {}
};

//  scan_qr_code 
class ScanQrCodeTool : public BridgeTool {
public:
    explicit ScanQrCodeTool(AndroidToolBridge bridge)
        : BridgeTool("scan_qr_code",
                     "Scan a QR code or barcode with the camera. Only when the user explicitly asks to scan one",
                     std::move(bridge)) {
        addParameter({"timeout_ms", "How long to wait for a code in milliseconds (default 15000)", "integer", false});
    }
};
