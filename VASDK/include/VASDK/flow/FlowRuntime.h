#pragma once

#include "vasdk_export.h"
#include "CompiledAgent.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace flow {

struct PageChange {
    std::string fromFlow, fromPage;   // empty when the conversation starts
    std::string toFlow, toPage;
    std::string reason;               // "start", "restart", "route:<intent>", "goto"
};

/** What one turn produced. */
struct Reply {
    std::string text;              // what to say; empty when nothing is said
    bool matched = true;           // false: no route understood the turn; text is the not-understood reply
    std::string intent;            // the matched route's intent
    bool expectsReply = false;     // the reply asks a question
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
public:
    explicit FlowRuntime(std::shared_ptr<const CompiledAgent> agent);

    /** Enters the start page and returns its greeting. Also starts over after an end. */
    Reply start();
    /**
     * Routes the user's words on the current page: the matched route's steps run, then the
     * conversation moves to its target. Not understood: "Sorry, I can't help with that here." and the
     * page's last question again. Before start(), or after the conversation ended, it starts
     * over at the start page without the greeting and routes the words there.
     */
    Reply handleText(const std::string& text);

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
    /** What $context.* reads, asked each time a reference needs it (it may change any time). */
    using ContextSource = std::function<google::protobuf::Struct()>;
    void setContextSource(ContextSource source) { _context = std::move(source); }
    /** The value a reference resolves to now ($session.tier, $page.locals.x, $input.text, ...). */
    Value lookup(const Path& path) const;

    /** Pages entered in one turn before it stops (a goto loop). */
    static constexpr int kMaxTransitions = 8;

private:
    struct Outcome {
        std::optional<CompiledAgent::PageRef> jump;
        bool ended = false;
    };
    Outcome run(const google::protobuf::RepeatedPtrField<vasdk::flow::Step>& steps, Reply& reply);
    std::string say(const vasdk::flow::Say& say, Reply& reply);
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
    ContextSource _context;
    std::string _lastQuestion;                              // repeated when an answer isn't understood
    std::map<const vasdk::flow::Say*, size_t> _sayCounts;   // which of a Say's texts comes next
};

} // namespace flow
} // namespace va
