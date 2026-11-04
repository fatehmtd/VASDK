#pragma once

#include "vasdk_export.h"
#include "CompiledAgent.h"

#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace flow {

struct PageChange {
    std::string fromFlow, fromPage;   // empty when the conversation starts
    std::string toFlow, toPage;
    std::string reason;               // "start", "restart", "route:<intent>", "goto", "return" (END_FLOW)
};

/** A part of a reply that is spoken differently from the parts around it. */
struct ReplySegment {
    std::string text;
    std::string ssml;              // for a voice that reads SSML; empty: speak text
    bool interruptible = true;     // false: the user can't talk over it
};

/** An answer offered with a question, for the app to show as a button. */
struct ReplyChoice {
    std::string label;
    std::string value;             // what the button sends back as the user's words
};

/** What one turn produced. */
struct Reply {
    std::string text;              // what to say; empty when nothing is said
    std::vector<ReplySegment> segments;   // text in parts, joined with spaces; one part when all are alike
    std::vector<ReplyChoice> choices;     // the question's choices
    google::protobuf::Struct ui;          // what the reply's `ui` gave the app
    bool matched = true;           // false: no route understood the turn; text is the not-understood reply
    std::string intent;            // the matched route's intent
    std::string event;             // no-match, no-input or max-reprompts, when the turn raised one
    bool expectsReply = false;     // the reply asks a question
    vasdk::flow::Expectation expectation;   // what the user is expected to say next; mode NONE when nothing was asked
    std::optional<int> followUpListenMs;    // how long the mic stays open after it, by the agent; unset: the engine's setting
    bool endsSession = false;      // the conversation ended with this reply
    std::vector<PageChange> pageChanges;
};

/**
 * @brief Where one conversation is in an agent, and what each user turn does there.
 *
 * Not thread-safe: one per conversation, used from one thread. Copyable, so a turn can be
 * undone by restoring the copy taken before it.
 *
 * @code
 * va::flow::FlowRuntime conversation(agent);
 * conversation.start().text;               // "Hi, this is Acme Garage. Book, cancel, or hours?"
 * conversation.handleText("book please");  // routes on the start page, enters the next one
 * @endcode
 */
class VASDK_EXPORT FlowRuntime {
    friend class TurnContext;
public:
    explicit FlowRuntime(std::shared_ptr<const CompiledAgent> agent);

    /** Enters the start page and returns its greeting. Also starts over after an end. */
    Reply start();
    /**
     * Routes the user's words on the current page: the matched route's steps run, then the
     * conversation moves to its target. Not understood: "Sorry, I can't help with that here." and the
     * page's last question again. On a form page the words answer the form first (Page.form).
     * Before start(), or after the conversation ended, it starts over at the start page without
     * the greeting and routes the words there.
     */
    Reply handleText(const std::string& text);
    /**
     * The user said nothing after a question, for noInputMs(): the no-input handler runs (or
     * max-reprompts after too many misses in a row); without one, the question is asked again.
     */
    Reply handleNoInput();
    /** How long to wait for an answer before handleNoInput(), from the page's settings; 0: never. */
    int noInputMs() const;
    /**
     * The user cut the last reply off before its end. A read-back cut off isn't confirmed by a
     * plain "yes" in the next turn: it's read back again. Any other answer counts as usual.
     */
    void replyInterrupted() { _replyCut = true; }
    /**
     * The user tapped the last reply's choice at `index` (from 0): the same as saying its value.
     * Out of range, nothing happens: the reply is empty and not matched.
     */
    Reply selectChoice(int index);

    bool started() const { return _position.valid(); }
    bool ended() const { return _ended; }
    std::string flowName() const;
    std::string pageName() const;

    /**
     * Sets a declared session param ($session.<name>). A number or boolean given for a STRING
     * param, or a numeric string for a NUMBER one, is converted; false, with `error` set, for an
     * undeclared name or a value that doesn't fit. Values stay for the whole conversation, also
     * after it ends and starts over.
     */
    bool setSessionParam(const std::string& name, const Value& value, std::string* error = nullptr);
    /** A session param's value; null when unset or undeclared. */
    Value sessionParam(const std::string& name) const;
    /**
     * Replaces a dynamic entity's values for this conversation (the times still free today, the
     * caller's cars). False, with `error` set, for an entity that isn't a dynamic list.
     */
    bool setEntityValues(const std::string& name, std::vector<vasdk::nlu::EntityValue> values,
                         std::string* error = nullptr);
    /** What $context.* reads, asked each time a reference needs it (it may change any time). */
    using ContextSource = std::function<google::protobuf::Struct()>;
    void setContextSource(ContextSource source) { _context = std::move(source); }
    /** The value a reference resolves to now ($session.tier, $form.day, $nlu.entities.service, ...). */
    Value lookup(const Path& path) const { return resolve(path, false); }

    /** Pages entered in one turn before it stops (a goto loop). */
    static constexpr int kMaxTransitions = 8;
    /** Flows entered with "flow:<name>" and not yet ended; deeper, the place to return to isn't kept. */
    static constexpr size_t kMaxFlowDepth = 8;

private:
    struct Outcome {
        std::optional<CompiledAgent::PageRef> jump;
        bool ended = false;
        std::string reason = "goto";
        bool pushFlow = false;   // entering a flow: remember where to return
        bool endFlow = false;    // END_FLOW: return there
        bool moves() const { return ended || endFlow || jump.has_value(); }
    };
    // The current page's form: its values and where it is in asking for them.
    struct Form {
        Form();
        Form(const Form& other);
        Form& operator=(const Form& other);
        Form(Form&& other) noexcept;
        Form& operator=(Form&& other) noexcept;
        ~Form();
        const vasdk::goal::GoalDefinition* goal = nullptr;
        std::unique_ptr<goal::GoalState> state;
        std::string asking;                 // the slot just asked for
        bool awaitingConfirmation = false;  // the values were read back: yes or no?
        bool awaitingChange = false;        // "no": what to change?
        bool failed = false;                // more answers that didn't fit than max_reprompts
        int reprompts = 0;
    };
    using Texts = google::protobuf::RepeatedPtrField<std::string>;

    Value resolve(const Path& path, bool spoken) const;   // spoken: entity values as replies say them
    void store(const std::string& key, const Value& value);
    std::optional<std::string> readSlot(const vasdk::goal::Slot& slot, const std::string& text) const;
    Outcome run(const google::protobuf::RepeatedPtrField<vasdk::flow::Step>& steps, Reply& reply);
    const vasdk::flow::Route* matchRoute();
    const vasdk::flow::Route* matchRoute(const google::protobuf::RepeatedPtrField<vasdk::flow::Route>& routes,
                                         const google::protobuf::RepeatedPtrField<std::string>* intents);
    Outcome toTarget(const std::string& target, std::string reason) const;
    Outcome takeRoute(const vasdk::flow::Route& route, Reply& reply);
    const vasdk::flow::Handler* findHandler(const std::string& event) const;
    int maxReprompts() const;
    Outcome miss(const std::string& event, Reply& reply);
    Outcome runHandler(const vasdk::flow::Handler& handler, Reply& reply);
    void askAgain(Reply& reply);
    void takeTurn(const std::string& text, Reply& reply);
    Outcome fromContext(const TurnContext& turn) const;
    Outcome answered(Outcome outcome, Reply& reply);
    void runAfterEnvelopes(Reply& reply);
    void finish(Reply& reply);
    vasdk::flow::Expectation formExpectation() const;
    std::vector<ReplyChoice> slotChoices(const vasdk::goal::Slot& slot) const;
    const vasdk::goal::Slot* askedSlot() const;
    size_t digitCount(const std::string& text) const;
    std::optional<std::string> readAnswer(const vasdk::goal::Slot& slot, const std::string& text) const;
    void pushFrame();
    void returnFromFlow(Reply& reply);
    void follow(Outcome outcome, Reply& reply);
    std::string say(const vasdk::flow::Say& say, Reply& reply);
    void offerChoices(Reply& reply) const;
    std::string chosenValue(const std::string& text) const;
    std::string render(const std::string& text) const;
    std::string pick(const Texts& texts, const void* key);   // the next of the texts, rendered
    std::string sayOne(const Texts& texts, const void* key, Reply& reply);
    std::string ask(const Texts& texts, const void* key, const std::string& fallback, Reply& reply);
    bool formActive() const;
    bool fillForm(const std::string& text);
    bool fillNamedValues(const std::string& text, bool overwrite);
    Outcome advanceForm(Reply& reply);
    Outcome repromptForm(Reply& reply);
    Outcome conditionRoute(Reply& reply);
    PageChange changeTo(CompiledAgent::PageRef target, std::string reason) const;
    void moveTo(CompiledAgent::PageRef target, std::string reason, Reply& reply);
    void end(Reply& reply);
    bool holds(const std::string& condition) const;
    Value evaluate(const std::string& expression) const;

    std::shared_ptr<const CompiledAgent> _agent;
    CompiledAgent::PageRef _position;
    bool _ended = false;
    google::protobuf::Struct _session;      // declared params, with their defaults
    google::protobuf::Struct _pageLocals;   // cleared on entering a page
    std::string _inputText;                 // the user's words this turn
    std::string _intent;                    // the matched route's intent this turn
    std::optional<EntityMatch> _entity;     // the matched route's entity this turn ($entity)
    DynamicEntityValues _dynamicEntities;
    ContextSource _context;
    std::optional<Form> _form;              // set on a page with a form
    // Where to return to on END_FLOW: the page that entered a flow, as it was.
    struct Frame {
        CompiledAgent::PageRef page;
        std::string lastQuestion;
        std::vector<vasdk::flow::Choice> questionChoices;
        vasdk::flow::Expectation expectation;
        std::optional<Form> form;
        google::protobuf::Struct pageLocals;
    };
    std::vector<Frame> _stack;
    int _misses = 0;                        // no-match and no-input in a row on this page
    std::string _lastQuestion;                              // repeated when an answer isn't understood
    std::vector<vasdk::flow::Choice> _questionChoices;      // its choices
    vasdk::flow::Expectation _expectation;                  // what the last question expects; cleared on entering a page
    std::vector<ReplyChoice> _choices;                      // the last reply's choices, for selectChoice
    bool _replyCut = false;                                 // the last reply was cut off (replyInterrupted)
    std::map<const void*, size_t> _sayCounts;               // which of a Say's (or prompt's) texts comes next
};

} // namespace flow
} // namespace va
