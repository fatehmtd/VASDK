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
