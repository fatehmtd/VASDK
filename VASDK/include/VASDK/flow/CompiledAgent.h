#pragma once

#include "vasdk_export.h"
#include "Entities.h"
#include "Expression.h"
#include "vasdk/flow/agent.pb.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace goal { struct GoalDefinition; class GoalState; }
namespace flow {

/**
 * @brief An agent checked and indexed for running. Immutable: every session that runs it
 *        shares one (Session::Options::agent), and each keeps its own FlowRuntime.
 *
 * @code
 * std::string error;
 * auto agent = va::flow::CompiledAgent::load("agents/garage-menu/agent.txtpb", &error);
 * if (!agent) spdlog::error(error);   // "flow \"main\" page \"welcome\" route 1: unknown intent \"bok\""
 * @endcode
 */
class VASDK_EXPORT CompiledAgent {
public:
    /** Where a conversation is: indexes into definition().flows() and that flow's pages(). */
    struct PageRef {
        int flow = -1;
        int page = -1;
        bool valid() const { return flow >= 0 && page >= 0; }
        bool operator==(const PageRef& other) const { return flow == other.flow && page == other.page; }
    };

    /**
     * Checks `agent`. nullptr, with every problem in `error` (one per line), when it can't run:
     * a missing start flow or page, an unknown intent or target, duplicate names. Problems that
     * don't stop it (an unreachable page, an unused intent) go to `warnings`.
     */
    static std::shared_ptr<const CompiledAgent> compile(vasdk::flow::Agent agent, std::string* error = nullptr,
                                                        std::vector<std::string>* warnings = nullptr);
    /** Loads a .txtpb, .json or .pb agent file and its goal_files, then compile(). */
    static std::shared_ptr<const CompiledAgent> load(const std::string& path, std::string* error = nullptr,
                                                     std::vector<std::string>* warnings = nullptr);

    const vasdk::flow::Agent& definition() const { return _agent; }
    PageRef startPage() const { return _startPage; }
    const vasdk::flow::Flow& flow(PageRef where) const { return _agent.flows(where.flow); }
    const vasdk::flow::Page& page(PageRef where) const { return _agent.flows(where.flow).pages(where.page); }

    /** A page of `from`'s flow, "flow:<name>" or "CURRENT_PAGE"; nullopt otherwise (END_SESSION too). */
    std::optional<PageRef> resolve(const std::string& target, PageRef from) const;
    /** The word count of the intent's longest example found in `words`, in order; 0 for none. */
    int matchIntent(const std::string& intent, const std::vector<std::string>& words) const;
    /** A declared session param, or nullptr. */
    const vasdk::flow::SessionParam* sessionParam(const std::string& name) const;
    /** An expression from the agent, parsed when it loaded; nullptr for text it doesn't hold. */
    std::shared_ptr<const Expression> expression(const std::string& text) const;
    /** A goal from goals or goal_files, or nullptr. */
    const vasdk::goal::GoalDefinition* goal(const std::string& name) const;
    /** A goal's slots, empty, ready for a form to fill; nullptr for an unknown goal. */
    std::unique_ptr<goal::GoalState> newGoalState(const std::string& name) const;
    /** The agent's entities, and the @sys.* ones. */
    const EntityCatalog& entities() const { return _entities; }

private:
    CompiledAgent() = default;

    vasdk::flow::Agent _agent;
    PageRef _startPage;
    std::map<std::string, int> _flows;
    std::vector<std::map<std::string, int>> _pages;   // per flow: page name → index
    std::map<std::string, std::vector<std::vector<std::string>>> _intentExamples;   // split into words
    std::map<std::string, int> _sessionParams;                                       // name → index
    std::map<std::string, std::shared_ptr<const Expression>> _expressions;           // by source text
    std::map<std::string, int> _goals;                                               // name → index
    std::map<std::string, std::shared_ptr<const goal::GoalDefinition>> _goalDefinitions;   // as GoalState runs them
    EntityCatalog _entities;
};

/** Whether `value` fits a session param of `type`; null always does. */
VASDK_EXPORT bool fitsType(vasdk::flow::SessionParam::Type type, const Value& value);

/** Route and Goto targets with a meaning of their own. */
inline constexpr const char* kTargetEndSession = "END_SESSION";
inline constexpr const char* kTargetCurrentPage = "CURRENT_PAGE";

/** "@service" → "service"; "" when it doesn't start with @. */
VASDK_EXPORT std::string entityName(const std::string& reference);

/** "Book an oil-change!" → {"book", "an", "oil", "change"}: lower case, punctuation dropped. */
VASDK_EXPORT std::vector<std::string> splitWords(const std::string& text);

} // namespace flow
} // namespace va
