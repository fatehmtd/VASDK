#include <VASDK/orchestrator/OrchestratorConfigLoader.h>

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace va {
namespace orchestrator {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("[OrchestratorConfigLoader] Cannot open: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Agent parseTextprotoAgent(const std::string& path) {
    std::string text = readFile(path);
    Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        throw std::runtime_error("[OrchestratorConfigLoader] Failed to parse textproto: " + path);
    }
    return agent;
}

} // namespace

//  Loading

Agent OrchestratorConfigLoader::fromTextprotoFile(const std::string& path) {
    Agent agent = parseTextprotoAgent(path);
    spdlog::debug("[OrchestratorConfigLoader] Loaded agent '{}' from '{}'", agent.id(), path);
    return agent;
}

Agent OrchestratorConfigLoader::fromBinaryFile(const std::string& path) {
    std::string bytes = readFile(path);
    Agent agent;
    if (!agent.ParseFromString(bytes)) {
        throw std::runtime_error("[OrchestratorConfigLoader] Failed to parse binary proto: " + path);
    }
    spdlog::debug("[OrchestratorConfigLoader] Loaded agent '{}' from '{}'", agent.id(), path);
    return agent;
}

Agent OrchestratorConfigLoader::fromDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    fs::path root(dirPath);
    fs::path agentFile = root / "agent.textproto";
    if (!fs::exists(agentFile)) {
        throw std::runtime_error("[OrchestratorConfigLoader] Missing agent.textproto in: " + dirPath);
    }

    // agent.textproto's top-level message IS an Agent (spec §14.1) - unwrapped, not
    // nested under an `agent { ... }` block. flows left empty here; composed below.
    Agent agent = parseTextprotoAgent(agentFile.string());

    std::unordered_set<std::string> flowIds;
    for (const auto& flow : agent.flows()) flowIds.insert(flow.id());

    fs::path flowsDir = root / "flows";
    if (fs::exists(flowsDir) && fs::is_directory(flowsDir)) {
        std::vector<fs::path> flowFiles;
        for (const auto& entry : fs::directory_iterator(flowsDir)) {
            if (entry.path().extension() == ".textproto") flowFiles.push_back(entry.path());
        }
        std::sort(flowFiles.begin(), flowFiles.end());  // lexical by filename, per spec §14.1

        for (const auto& path : flowFiles) {
            std::string flowText = readFile(path.string());
            Flow flow;
            if (!google::protobuf::TextFormat::ParseFromString(flowText, &flow)) {
                throw std::runtime_error("[OrchestratorConfigLoader] Failed to parse: " + path.string());
            }
            if (flowIds.count(flow.id())) {
                throw std::runtime_error("[OrchestratorConfigLoader] Duplicate flow id '" + flow.id() +
                                          "' - already defined before " + path.string());
            }
            flowIds.insert(flow.id());
            *agent.add_flows() = std::move(flow);
        }
    }

    spdlog::debug("[OrchestratorConfigLoader] Composed agent '{}' from '{}' ({} flow(s))",
                  agent.id(), dirPath, agent.flows_size());
    return agent;
}

//  validate()

namespace {

// spec §9.5 - the three ways an Executor's effects can span into a future turn.
bool executorHasKind(const Executor& ex, Action::KindCase kind) {
    for (const auto& action : ex.actions()) {
        if (action.kind_case() == kind) return true;
    }
    return false;
}

bool nodeHasRequiredParameter(const Node& node) {
    for (const auto& p : node.form()) {
        if (p.required()) return true;
    }
    return false;
}

// Dead end: no route offers a transition, emits an event, hands off, or
// offers a turn-spanning mechanism, and the node itself doesn't turn-span on
// entry (a required form parameter or a generative loop).
//
// `handoff` is treated as exempting even though spec §9.5's own enumeration
// only names Transition/emitEvent/the three turn-spanning mechanisms - a
// literal reading would flag the spec's own canonical triage flow (§14/§19,
// `confirm_urgent`: entry says a line, then a node_enter route whose executor
// is just `handoff`, no transition) as a dead end. A deliberate handoff ends
// the conversation on purpose; it isn't the silent hang §9.5 is about.
// Milestone 2.5 (spec §9.5 "Placement constraint") - `expect` ends the turn
// the instant it runs (OrchestratorEngine::runAction's Action::kExpect case),
// so nothing declared after it in the same Executor can ever run. Flags the
// first offending Executor only per call site - enough to point the author at
// the fix without drowning them in every downstream action.
void checkExpectPlacement(const std::string& location, const Executor& ex, ValidationReport& report) {
    for (int i = 0; i < ex.actions_size(); ++i) {
        if (ex.actions(i).kind_case() != Action::kExpect) continue;
        if (i != ex.actions_size() - 1) {
            report.errors.push_back(
                location + ": has an action after `expect`, which can never run - "
                "expect must be the last action in its Executor (spec §9.5)");
        }
        break; // at most one `expect` is meaningful per Executor - anything past the first is already unreachable.
    }
}

bool nodeIsDeadEnd(const Node& node) {
    if (node.generative()) return false;
    if (nodeHasRequiredParameter(node)) return false;
    for (const auto& route : node.routes()) {
        if (route.has_transition()) return false;
        if (executorHasKind(route.executor(), Action::kEmitEvent)) return false;
        if (executorHasKind(route.executor(), Action::kExpect)) return false;
        if (executorHasKind(route.executor(), Action::kHandoff)) return false;
    }
    return true;
}

void validateFlow(const Agent& agent, const Flow& flow,
                   const std::unordered_set<std::string>& flowIds, ValidationReport& report) {
    std::unordered_map<std::string, const Node*> nodesById;
    for (const auto& node : flow.nodes()) {
        if (!nodesById.emplace(node.id(), &node).second) {
            report.errors.push_back("Flow '" + flow.id() + "': duplicate node id '" + node.id() + "'");
        }
    }

    if (flow.start_node_id().empty()) {
        report.errors.push_back("Flow '" + flow.id() + "': start_node_id is empty");
    } else if (!nodesById.count(flow.start_node_id())) {
        report.errors.push_back("Flow '" + flow.id() + "': start_node_id '" + flow.start_node_id() +
                                 "' does not resolve to a node in this flow");
    }

    // Referential integrity for every route's transition, independent of reachability.
    for (const auto& [nodeId, nodePtr] : nodesById) {
        for (const auto& route : nodePtr->routes()) {
            if (!route.has_transition()) continue;
            const Transition& t = route.transition();
            switch (t.target_type()) {
                case Transition::NODE:
                    if (t.target_id().empty()) {
                        report.errors.push_back("Flow '" + flow.id() + "', node '" + nodeId + "', route '" +
                                                 route.id() + "': NODE transition has empty target_id");
                    } else if (!nodesById.count(t.target_id())) {
                        report.errors.push_back("Flow '" + flow.id() + "', node '" + nodeId + "', route '" +
                                                 route.id() + "': transition target_id '" + t.target_id() +
                                                 "' does not resolve to a node in this flow");
                    }
                    break;
                case Transition::FLOW:
                    if (t.target_id().empty()) {
                        report.errors.push_back("Flow '" + flow.id() + "', node '" + nodeId + "', route '" +
                                                 route.id() + "': FLOW transition has empty target_id");
                    } else if (!flowIds.count(t.target_id())) {
                        report.errors.push_back("Flow '" + flow.id() + "', node '" + nodeId + "', route '" +
                                                 route.id() + "': transition target_id '" + t.target_id() +
                                                 "' does not resolve to any flow in this agent");
                    }
                    break;
                case Transition::END_SESSION:
                case Transition::NONE:
                    break;
                default:
                    // protobuf's closed-enum sentinel values (*_INT_MIN_SENTINEL_DO_NOT_USE_,
                    // *_INT_MAX_SENTINEL_DO_NOT_USE_) - never a real TargetType, nothing to do.
                    break;
            }
        }
    }

    // Reachability: BFS over NODE-transitions from start_node_id, valid targets only.
    // Conservative by construction - see the ValidationReport doc comment for why this
    // is a warning, not an error.
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& [nodeId, nodePtr] : nodesById) {
        for (const auto& route : nodePtr->routes()) {
            if (!route.has_transition()) continue;
            const Transition& t = route.transition();
            if (t.target_type() == Transition::NODE && nodesById.count(t.target_id())) {
                adjacency[nodeId].push_back(t.target_id());
            }
        }
    }

    std::unordered_set<std::string> reachable;
    if (nodesById.count(flow.start_node_id())) {
        std::deque<std::string> queue{flow.start_node_id()};
        reachable.insert(flow.start_node_id());
        while (!queue.empty()) {
            std::string current = queue.front();
            queue.pop_front();
            for (const auto& next : adjacency[current]) {
                if (reachable.insert(next).second) queue.push_back(next);
            }
        }
    }

    for (const auto& [nodeId, nodePtr] : nodesById) {
        (void)nodePtr;
        if (!reachable.count(nodeId)) {
            report.warnings.push_back("Flow '" + flow.id() + "': node '" + nodeId +
                                       "' is not reachable from start_node_id '" + flow.start_node_id() +
                                       "' via any static transition (may still be reachable via an "
                                       "ambient trigger or a webhook response's dynamic transition)");
        }
    }

    // spec §9.5 dead-end check.
    for (const auto& [nodeId, nodePtr] : nodesById) {
        if (nodeIsDeadEnd(*nodePtr)) {
            report.errors.push_back(
                "Flow '" + flow.id() + "': node '" + nodeId +
                "' is a dead end - no route has a transition, emits an event, or offers a "
                "turn-spanning mechanism (required form parameter, expect action, or generative node)");
        }
    }

    // spec §9.5 "Placement constraint" - expect must be the last action in
    // its Executor, whether that's a node's entry_executor or a route's.
    for (const auto& [nodeId, nodePtr] : nodesById) {
        checkExpectPlacement("Flow '" + flow.id() + "', node '" + nodeId + "' entry_executor",
                             nodePtr->entry_executor(), report);
        for (const auto& route : nodePtr->routes()) {
            checkExpectPlacement("Flow '" + flow.id() + "', node '" + nodeId + "', route '" + route.id() + "'",
                                 route.executor(), report);
        }
    }

    // Handler coverage (best-effort, non-blocking): a permission-gated route or a
    // webhook call with no fallback handler anywhere in the node -> flow -> agent chain.
    bool flowHasPermissionDenied = flow.flow_handlers().has_permission_denied();
    bool agentHasPermissionDenied = agent.global_handlers().has_permission_denied();
    bool flowHasWebhookError = flow.flow_handlers().has_webhook_error();
    bool agentHasWebhookError = agent.global_handlers().has_webhook_error();

    for (const auto& [nodeId, nodePtr] : nodesById) {
        bool nodeHasPermissionDenied = nodePtr->node_handlers().has_permission_denied();
        bool nodeHasWebhookError = nodePtr->node_handlers().has_webhook_error();

        for (const auto& route : nodePtr->routes()) {
            if (!route.required_role().empty() && !nodeHasPermissionDenied &&
                !flowHasPermissionDenied && !agentHasPermissionDenied) {
                report.warnings.push_back(
                    "Flow '" + flow.id() + "', node '" + nodeId + "', route '" + route.id() +
                    "': required_role is set but no permission_denied handler exists on this "
                    "node, its flow, or the agent");
            }
            if (executorHasKind(route.executor(), Action::kCallWebhook) && !nodeHasWebhookError &&
                !flowHasWebhookError && !agentHasWebhookError) {
                report.warnings.push_back(
                    "Flow '" + flow.id() + "', node '" + nodeId + "', route '" + route.id() +
                    "': calls a webhook but no webhook_error handler exists on this node, its "
                    "flow, or the agent");
            }
        }
    }
}

} // namespace

ValidationReport OrchestratorConfigLoader::validate(const Agent& agent) {
    ValidationReport report;

    std::unordered_set<std::string> flowIds;
    for (const auto& flow : agent.flows()) {
        if (!flowIds.insert(flow.id()).second) {
            report.errors.push_back("Duplicate flow id: '" + flow.id() + "'");
        }
    }

    for (const auto& flow : agent.flows()) {
        validateFlow(agent, flow, flowIds, report);
    }

    report.ok = report.errors.empty();
    return report;
}

} // namespace orchestrator
} // namespace va
