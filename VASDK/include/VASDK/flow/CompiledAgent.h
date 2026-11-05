#pragma once

#include "vasdk_export.h"
#include "Entities.h"
#include "Expression.h"
#include "FlowRegistry.h"
#include "../nlu/RuleNluProvider.h"
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
                                                        std::vector<std::string>* warnings = nullptr,
                                                        std::shared_ptr<const FlowRegistry> registry = nullptr);
    /**
     * Loads a .txtpb, .json or .pb agent file, its proto_imports and goal_files, then compile().
     * `registry` holds the C++ steps, page types and envelopes it names; nullptr: the built-in
     * page types only.
     */
    static std::shared_ptr<const CompiledAgent> load(const std::string& path, std::string* error = nullptr,
                                                     std::vector<std::string>* warnings = nullptr,
                                                     std::shared_ptr<const FlowRegistry> registry = nullptr);

    const vasdk::flow::Agent& definition() const { return _agent; }
    PageRef startPage() const { return _startPage; }
    const vasdk::flow::Flow& flow(PageRef where) const { return _agent.flows(where.flow); }
    const vasdk::flow::Page& page(PageRef where) const { return _agent.flows(where.flow).pages(where.page); }

    /** A page of `from`'s flow, "flow:<name>" or "CURRENT_PAGE"; nullopt otherwise (END_SESSION too). */
    std::optional<PageRef> resolve(const std::string& target, PageRef from) const;
    /** Understands the user's intents from the agent's examples (and the built-in sys.* ones). */
    std::shared_ptr<const nlu::RuleNluProvider> rules() const { return _rules; }
    /** A declared or built-in (sys.*) intent. */
    bool knowsIntent(const std::string& name) const;
    /** A declared session param, or nullptr. */
    const vasdk::flow::SessionParam* sessionParam(const std::string& name) const;
    /** An expression from the agent, parsed when it loaded; nullptr for text it doesn't hold. */
    std::shared_ptr<const Expression> expression(const std::string& text) const;
    /** A goal from goals or goal_files, or nullptr. */
    const vasdk::goal::GoalDefinition* goal(const std::string& name) const;
    /** A goal's slots, empty, ready for a form to fill; nullptr for an unknown goal. */
    std::unique_ptr<goal::GoalState> newGoalState(const std::string& name) const;
    /** What a page's turns are matched against, innermost first: the page's own routes and handlers,
     *  then its envelopes, its flow's and the agent's (each followed by what it extends). */
    struct Layer {
        const google::protobuf::RepeatedPtrField<vasdk::flow::Route>* routes;
        const google::protobuf::RepeatedPtrField<vasdk::flow::Handler>* handlers;
        const vasdk::flow::Settings* settings;   // nullptr for the page's own layer
    };
    const std::vector<Layer>& layers(PageRef where) const { return _layers[where.flow][where.page]; }
    /** The agent's entities, and the @sys.* ones. */
    const EntityCatalog& entities() const { return _entities; }

    /** The C++ steps, page types and envelopes the agent was loaded with. */
    const FlowRegistry& registry() const { return *_registry; }
    /** A custom step's params, read when the agent loaded; nullptr for an Any it doesn't hold. */
    const google::protobuf::Message* params(const google::protobuf::Any& params) const;
    /** What a page's type and the C++ envelopes applied to it add to it. */
    struct PageExtensions {
        const vasdk::flow::PageType* declaredType = nullptr;      // from page_types
        const FlowRegistry::PageType* registeredType = nullptr;   // written in C++
        const google::protobuf::Message* params = nullptr;        // the page's params
        Value paramsValue;                                        // the same, as $params reads them
        std::vector<const FlowRegistry::Envelope*> envelopes;     // written in C++, innermost first
    };
    const PageExtensions& extensions(PageRef where) const { return _extensions[where.flow][where.page]; }

private:
    struct ImportedTypes;
    CompiledAgent() = default;
    static std::shared_ptr<const CompiledAgent> build(vasdk::flow::Agent agent, std::shared_ptr<const ImportedTypes> imports,
                                                      std::shared_ptr<const FlowRegistry> registry, std::string* error,
                                                      std::vector<std::string>* warnings);

    vasdk::flow::Agent _agent;
    PageRef _startPage;
    std::map<std::string, int> _flows;
    std::vector<std::map<std::string, int>> _pages;   // per flow: page name → index
    std::shared_ptr<const nlu::RuleNluProvider> _rules;
    std::map<std::string, int> _sessionParams;                                       // name → index
    std::map<std::string, std::shared_ptr<const Expression>> _expressions;           // by source text
    std::map<std::string, int> _goals;                                               // name → index
    std::map<std::string, std::shared_ptr<const goal::GoalDefinition>> _goalDefinitions;   // as GoalState runs them
    EntityCatalog _entities;
    std::map<std::string, int> _envelopes;                                           // name → index
    std::vector<std::vector<std::vector<Layer>>> _layers;                            // per flow, per page
    std::shared_ptr<const ImportedTypes> _imports;                                   // proto_imports' types
    std::shared_ptr<const FlowRegistry> _registry;
    std::map<const google::protobuf::Any*, std::shared_ptr<const google::protobuf::Message>> _params;   // by where they are in _agent
    std::vector<std::vector<PageExtensions>> _extensions;                            // per flow, per page
};

/** Whether `value` fits a session param of `type`; null always does. */
VASDK_EXPORT bool fitsType(vasdk::flow::SessionParam::Type type, const Value& value);

/** Route and Goto targets with a meaning of their own. */
inline constexpr const char* kTargetEndSession = "END_SESSION";
inline constexpr const char* kTargetCurrentPage = "CURRENT_PAGE";
inline constexpr const char* kTargetEndFlow = "END_FLOW";
/** A route's: the session's LLM conversation answers the turn (Reply::toLlm); the route's steps are
 *  the answer without one, and the conversation stays on the page. */
inline constexpr const char* kTargetLlm = "LLM";

/** Handler events. */
inline constexpr const char* kEventNoMatch = "no-match";
inline constexpr const char* kEventNoInput = "no-input";
inline constexpr const char* kEventMaxReprompts = "max-reprompts";
inline constexpr const char* kEventToolError = "tool-error";
inline constexpr const char* kEventClarify = "clarify";

/** "@service" → "service"; "" when it doesn't start with @. */
VASDK_EXPORT std::string entityName(const std::string& reference);

/** "Book an oil-change!" → {"book", "an", "oil", "change"}: lower case, punctuation dropped. */
VASDK_EXPORT std::vector<std::string> splitWords(const std::string& text);

} // namespace flow
} // namespace va
