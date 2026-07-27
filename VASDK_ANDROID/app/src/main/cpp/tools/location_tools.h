#pragma once
#include "bridge_tool.h"

//  get_location 
class GetLocationTool : public BridgeTool {
public:
    explicit GetLocationTool(AndroidToolBridge bridge)
        : BridgeTool("get_location",
                     "Get the device's current GPS coordinates. Takes no arguments - no address or place name needed",
                     std::move(bridge)) {}
};

//  navigate_to 
class NavigateToTool : public BridgeTool {
public:
    explicit NavigateToTool(AndroidToolBridge bridge)
        : BridgeTool("navigate_to",
                     "Open the maps app and navigate to an address or place. Only when the user explicitly asks for directions",
                     std::move(bridge)) {
        addParameter({"address", "Destination address or place name (e.g. 'Eiffel Tower, Paris' or '10 Downing Street')", "string", true});
    }
};
