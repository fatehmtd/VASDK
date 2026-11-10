#pragma once

#include <VASDK/vasdk_export.h>
#include "../Masking.h"
#include "vasdk/engine/engine_config.pb.h"
#include "vasdk/flow/agent.pb.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace va {
namespace network { class HttpInterface; }
namespace flow {

class CompiledAgent;

/** What a tool call gave back: its result, or why it failed. */
struct ToolResult {
    bool ok = false;
    nlohmann::json value;    // the result: an object, or the reply's text when it isn't JSON
    std::string error;       // when !ok
};

/** Calls a tool by name with its args; what FlowRuntime's call_tool step uses. */
using ToolCaller = std::function<ToolResult(const std::string& name, const nlohmann::json& args)>;

/**
 * @brief Runs an agent's tools: those registered in C++ (FlowRegistry::addTool), else those the
 *        agent declares in `tools` (HTTP), within the engine config's security settings.
 *
 * An HTTP tool's URL must match security.http_allow; redirects aren't followed, certificates are
 * checked, and a reply longer than max_response_kb fails. A tool still running after timeout_ms
 * fails (a C++ one is left to finish on its own thread). Thread-safe.
 */
class VASDK_EXPORT ToolRunner {
public:
    /** `http`: for tests; unset, a libcurl client (when the build has one). */
    ToolRunner(std::shared_ptr<const CompiledAgent> agent, vasdk::engine::Security security = {},
               std::shared_ptr<network::HttpInterface> http = nullptr);

    ToolResult call(const std::string& name, const nlohmann::json& args) const;
    /** A ToolCaller for FlowRuntime::setToolCaller; keeps this runner alive. */
    static ToolCaller caller(std::shared_ptr<const ToolRunner> runner);

    /** Whether `url` matches one of `patterns` ("https://api.acme.example/*"; * matches anything). */
    static bool allowed(const google::protobuf::RepeatedPtrField<std::string>& patterns, const std::string& url);

private:
    ToolResult callHttp(const vasdk::flow::Tool& tool, const nlohmann::json& args) const;

    std::shared_ptr<const CompiledAgent> _agent;
    vasdk::engine::Security _security;
    std::shared_ptr<network::HttpInterface> _http;
};

// maskValue("555-123-4567") → "***-***-4567", and redact(text, values): VASDK/Masking.h.
using va::maskValue;
using va::redact;

} // namespace flow
} // namespace va
