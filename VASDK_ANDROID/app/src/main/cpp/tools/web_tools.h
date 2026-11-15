#pragma once
#include "bridge_tool.h"

//  web_search 
class WebSearchTool : public BridgeTool {
public:
    explicit WebSearchTool(AndroidToolBridge bridge)
        : BridgeTool("web_search",
                     "Search the web. Returns top results with title, snippet, and URL",
                     std::move(bridge)) {
        addParameter({"query", "Search query string", "string", true});
    }
};

//  fetch_url 
class FetchUrlTool : public BridgeTool {
public:
    explicit FetchUrlTool(AndroidToolBridge bridge)
        : BridgeTool("fetch_url",
                     "Fetch a URL and return the start of the response body",
                     std::move(bridge)) {
        addParameter({"url", "Full URL to fetch (must start with https://)", "string", true});
    }
};

//  get_weather 
class GetWeatherTool : public BridgeTool {
public:
    explicit GetWeatherTool(AndroidToolBridge bridge)
        : BridgeTool("get_weather",
                     "Get the current weather for a place: conditions, temperature, humidity and wind",
                     std::move(bridge)) {
        addParameter({"location", "The place: a city, a region or a country, e.g. \"Tokyo\"", "string", true});
        addParameter({"unit", "\"celsius\" (default) or \"fahrenheit\"", "string", false});
    }
};
