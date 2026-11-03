#include "flow/FlowRuntime.h"

#include <spdlog/spdlog.h>

namespace va {
namespace flow {

namespace {
constexpr const char* kNotUnderstood = "Sorry, I can't help with that here.";

void append(std::string& text, const std::string& more) {
    if (more.empty()) return;
    if (!text.empty()) text += " ";
    text += more;
}
} // namespace

FlowRuntime::FlowRuntime(std::shared_ptr<const CompiledAgent> agent) : _agent(std::move(agent)) {}

std::string FlowRuntime::flowName() const { return started() ? _agent->flow(_position).name() : std::string(); }
std::string FlowRuntime::pageName() const { return started() ? _agent->page(_position).name() : std::string(); }

Reply FlowRuntime::start() {
    Reply reply;
    _ended = false;
    moveTo(_agent->startPage(), "start", reply);
    return reply;
}

Reply FlowRuntime::handleText(const std::string& text) {
    Reply reply;
    if (!started() || _ended) {
        reply.pageChanges.push_back(changeTo(_agent->startPage(), started() ? "restart" : "start"));
        _position = _agent->startPage();
        _ended = false;
        _lastQuestion.clear();
    }

    const auto words = splitWords(text);
    const vasdk::flow::Route* best = nullptr;
    int bestScore = 0;
    for (const auto& route : _agent->page(_position).routes()) {
        const int score = _agent->matchIntent(route.intent(), words);
        if (score > bestScore) {   // ties: the first route wins
            best = &route;
            bestScore = score;
        }
    }
    if (!best) {
        reply.matched = false;
        reply.text = kNotUnderstood;
        append(reply.text, _lastQuestion);
        reply.expectsReply = !_lastQuestion.empty();
        return reply;
    }

    reply.intent = best->intent();
    const auto outcome = run(best->steps(), reply);
    if (outcome.ended || best->target() == kTargetEndSession) {
        end(reply);
    } else if (outcome.jump) {
        moveTo(*outcome.jump, "goto", reply);
    } else if (!best->target().empty()) {
        if (auto target = _agent->resolve(best->target(), _position)) moveTo(*target, "route:" + best->intent(), reply);
    }
    return reply;
}

FlowRuntime::Outcome FlowRuntime::run(const google::protobuf::RepeatedPtrField<vasdk::flow::Step>& steps,
                                      Reply& reply) {
    for (const auto& step : steps) {
        switch (step.kind_case()) {
        case vasdk::flow::Step::kSay:
            say(step.say(), reply);
            break;
        case vasdk::flow::Step::kAsk:
            _lastQuestion = say(step.ask().say(), reply);
            reply.expectsReply = true;
            break;
        case vasdk::flow::Step::kGoto: {
            const auto& target = step.goto_().target();
            if (target == kTargetEndSession) return {std::nullopt, true};
            return {_agent->resolve(target, _position), false};
        }
        case vasdk::flow::Step::kEnd:
            return {std::nullopt, true};
        case vasdk::flow::Step::KIND_NOT_SET:
            break;
        }
    }
    return {};
}

std::string FlowRuntime::say(const vasdk::flow::Say& say, Reply& reply) {
    if (say.text().empty()) return {};
    auto& count = _sayCounts[&say];
    const std::string& text = say.text(static_cast<int>(count++ % say.text_size()));
    append(reply.text, text);
    return text;
}

PageChange FlowRuntime::changeTo(CompiledAgent::PageRef target, std::string reason) const {
    PageChange change;
    change.fromFlow = flowName();
    change.fromPage = pageName();
    change.toFlow = _agent->flow(target).name();
    change.toPage = _agent->page(target).name();
    change.reason = std::move(reason);
    return change;
}

void FlowRuntime::moveTo(CompiledAgent::PageRef target, std::string reason, Reply& reply) {
    std::optional<CompiledAgent::PageRef> next = target;
    for (int transitions = 0; next; ++transitions) {
        if (transitions == kMaxTransitions) {
            spdlog::error("[FlowRuntime] {} pages entered in one turn (a goto loop?): staying on \"{}\"",
                          kMaxTransitions, pageName());
            return;
        }
        reply.pageChanges.push_back(changeTo(*next, reason));
        _position = *next;
        _lastQuestion.clear();
        const auto outcome = run(_agent->page(_position).on_enter(), reply);
        if (outcome.ended) {
            end(reply);
            return;
        }
        next = outcome.jump;
        reason = "goto";
    }
}

void FlowRuntime::end(Reply& reply) {
    _ended = true;
    reply.endsSession = true;
    reply.expectsReply = false;
}

} // namespace flow
} // namespace va
