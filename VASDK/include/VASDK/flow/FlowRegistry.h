#pragma once

#include "vasdk_export.h"
#include "Entities.h"
#include "Expression.h"
#include "../llm/Tool.h"
#include "../nlu/Language.h"
#include "vasdk/flow/agent.pb.h"
#include "vasdk/flow/step_outcome.pb.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace va {
namespace flow {

class FlowRuntime;
struct Reply;

/**
 * @brief What a step, page type or envelope written in C++ reads and does during a turn.
 *
 * Steps, page types and envelopes are shared by every conversation running the agent: keep what
 * a conversation needs in its values ($session.*, $page.locals.*), never in the C++ object.
 */
class VASDK_EXPORT TurnContext {
public:
    /** The value a reference holds now: "$session.tier", "$form.day", "$params.prompt"; null if unknown. */
    Value lookup(const std::string& reference) const;
    /** Stores a value, as a `set` step: "$session.<name>", "$page.locals.<name>" or "$form.<slot>". */
    void set(const std::string& key, const Value& value);
    /** What the user said this turn: on entering a page, the words that led there; empty at the start and on no-input. */
    const std::string& text() const;
    /** Changes what the rest of the turn reads as the user's words (an envelope's before: redact them). */
    void setText(std::string text);
    /** Adds `text` to the reply, as it is. */
    void say(const std::string& text);
    /** Fills in a template's references ("Welcome back, $session.name."), as a `say` does. */
    std::string render(const std::string& text) const;
    /** Asks a question, as an `ask` step: an answer is expected, and the question is asked again when it isn't understood. */
    void ask(const std::string& question, const std::vector<vasdk::flow::Choice>& choices = {},
             const vasdk::flow::Expectation& expect = {});
    /** Moves to `target` once this returns, as a `goto`: a page of this flow, "flow:<name>", END_FLOW, END_SESSION or CURRENT_PAGE. */
    void goTo(const std::string& target);
    /**
     * Does what a step written outside C++ asks (the C API, Kotlin): sets and unsets values, says
     * texts, moves. Keys other than $session.<declared param>, $page.locals.<name> and, on a form
     * page, $form.<slot>, values that don't fit, or an unknown target: false, `error` set, and
     * nothing is applied.
     */
    bool apply(const vasdk::flow::StepOutcome& outcome, std::string* error = nullptr);
    /** What the reply says so far. */
    const std::string& replyText() const;
    /** The agent's entities and languages: entities().languages().speaking() for the language replies are in. */
    const EntityCatalog& entities() const;
    std::string flowName() const;
    std::string pageName() const;

private:
    friend class FlowRuntime;
    TurnContext(FlowRuntime& runtime, Reply& reply) : _runtime(runtime), _reply(reply) {}

    FlowRuntime& _runtime;
    Reply& _reply;
    std::string _target;   // set by goTo
};

/**
 * @brief Steps, page types and envelopes written in C++, by name, for agents to use.
 *
 * Give it to CompiledAgent::load(); an agent that names something it doesn't hold fails to
 * load. Fill it before loading, then share it: lookups are read-only.
 *
 * @code
 * auto registry = std::make_shared<va::flow::FlowRegistry>();
 * registry->addStep<acme::LoyaltyPoints>([](const acme::LoyaltyPoints& params, va::flow::TurnContext& turn) {
 *     turn.set(params.into(), va::flow::numberValue(crm().points(turn.lookup("$session.customer").string_value())));
 * });
 * auto agent = va::flow::CompiledAgent::load("agent.txtpb", &error, nullptr, registry);
 * // agent.txtpb: on_enter { custom { [type.googleapis.com/acme.LoyaltyPoints] { into: "$page.locals.points" } } }
 * @endcode
 */
class VASDK_EXPORT FlowRegistry {
public:
    using Message = google::protobuf::Message;
    /** A step: what it does with its params during the turn. */
    using Step = std::function<void(const Message& params, TurnContext& turn)>;
    /** A page type: steps on entering the page, and the first say in each turn on it. */
    struct PageType {
        std::string paramsType;   // the full name of its params message; empty: none
        std::function<void(const Message& params, TurnContext& turn)> onEnter;
        // true: the turn is answered; false: the page's routes get it, as usual.
        std::function<bool(const Message& params, TurnContext& turn)> onTurn;
        // The targets the params name, checked when the agent loads and counted as reachable.
        std::function<std::vector<std::string>(const Message& params)> targets;
    };
    /** Behaviour around every turn of the pages it's applied to (`envelopes: ["<name>"]`). */
    struct Envelope {
        // Before the page's routes, outermost envelope first. true: the turn is answered.
        std::function<bool(TurnContext& turn)> before;
        // Once the reply is ready, innermost first: may add to it, store values.
        std::function<void(TurnContext& turn)> after;
    };

    /** With the built-in page types: menu, yes_no and end (vasdk/flow/page_types.proto). */
    FlowRegistry();
    /** The built-in page types only: what agents loaded without a registry use. */
    static std::shared_ptr<const FlowRegistry> defaults();

    /** A step for `custom` steps whose params are a `paramsType` message ("acme.LoyaltyPoints"). */
    void addStep(const std::string& paramsType, Step step);
    /** A step with its params as their own C++ type, generated from their .proto. */
    template <class Params, class Function>
    void addStep(Function step) {
        addStep(std::string(Params::descriptor()->full_name()), [step](const Message& params, TurnContext& turn) {
            withParams<Params>(params, [&](const Params& typed) { step(typed, turn); });
        });
    }
    /** A page type for pages with `type: "<name>"`; replaces one of the same name. */
    void addPageType(const std::string& name, PageType type);
    /** A page type with its params as their own C++ type. `onTurn` may be nullptr: the routes get every turn. */
    template <class Params, class Enter, class Turn>
    void addPageType(const std::string& name, Enter onEnter, Turn onTurn) {
        PageType type;
        type.paramsType = Params::descriptor()->full_name();
        type.onEnter = [onEnter](const Message& params, TurnContext& turn) {
            withParams<Params>(params, [&](const Params& typed) { onEnter(typed, turn); });
        };
        if constexpr (!std::is_same_v<Turn, std::nullptr_t>) {
            type.onTurn = [onTurn](const Message& params, TurnContext& turn) {
                bool answered = false;
                withParams<Params>(params, [&](const Params& typed) { answered = onTurn(typed, turn); });
                return answered;
            };
        }
        addPageType(name, std::move(type));
    }
    /** An envelope for `envelopes: ["<name>"]` on the agent, a flow or a page. */
    void addEnvelope(const std::string& name, Envelope envelope);
    /**
     * A tool for call_tool steps, by its name: its handler gets the step's args as a JSON object
     * and returns the result; {"error": "..."} or an exception fails the call (tool-error). It
     * runs on a thread of its own and may block, up to the engine config's security.timeout_ms.
     */
    void addTool(llm::Tool tool);

    /** What an entity extractor is given: the user's words as entities read them (entityWords), and the text. */
    struct EntityRequest {
        const std::vector<std::string>& words;
        const std::string& text;
    };
    /** A value it found: the words it was said in (indexes into words), what's stored and compared, how replies say it. */
    struct EntityFound {
        size_t start = 0;
        size_t length = 1;
        google::protobuf::Value value;
        std::string spoken;                    // empty: the value as text
        google::protobuf::Struct attributes;   // $form.<slot>.<attribute>
    };
    using EntityExtractor = std::function<std::vector<EntityFound>(const EntityRequest& request)>;
    /**
     * Finds the values of `code { extractor: "<name>" }` entities in the user's words: a lookup in
     * a database, the phone's contacts. Called on the conversation's thread, each time the entity
     * is looked for; it should be quick. A value outside the words is dropped.
     */
    void addEntityExtractor(const std::string& name, EntityExtractor extractor);

    /**
     * A language agents may name in locale or languages: a pack read from a file
     * (nlu::Language::load), or a subclass for grammar a pack can't express. One with the code of
     * another replaces it; an agent's own language_files and language_defs win over these.
     */
    void addLanguage(std::shared_ptr<const nlu::Language> language);

    const Step* step(const std::string& paramsType) const;
    const PageType* pageType(const std::string& name) const;
    const Envelope* envelope(const std::string& name) const;
    const llm::Tool* tool(const std::string& name) const;
    const EntityExtractor* entityExtractor(const std::string& name) const;
    const std::vector<std::shared_ptr<const nlu::Language>>& languages() const { return _languages; }

    /** Calls `use` with `params` as a Params: as it is when it's one, else converted (a type loaded with proto_imports). */
    template <class Params, class Use>
    static void withParams(const Message& params, Use use) {
        if (params.GetDescriptor() == Params::descriptor()) {
            use(static_cast<const Params&>(params));
            return;
        }
        Params typed;
        typed.ParseFromString(params.SerializeAsString());
        use(typed);
    }

private:
    std::map<std::string, Step> _steps;
    std::map<std::string, PageType> _pageTypes;
    std::map<std::string, Envelope> _envelopes;
    std::map<std::string, llm::Tool> _tools;
    std::map<std::string, EntityExtractor> _entityExtractors;
    std::vector<std::shared_ptr<const nlu::Language>> _languages;
};

} // namespace flow
} // namespace va
