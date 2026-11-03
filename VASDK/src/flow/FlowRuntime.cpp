#include "flow/FlowRuntime.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace va {
namespace flow {

namespace {
// "$session.tier" → {"session", "tier"}; set keys were checked when the agent loaded.
Path splitReference(const std::string& key) {
    auto expression = Expression::parse(key);
    const Path* path = expression ? expression->reference() : nullptr;
    return path ? *path : Path{};
}

constexpr const char* kNotUnderstood = "Sorry, I can't help with that here.";

void append(std::string& text, const std::string& more) {
    if (more.empty()) return;
    if (!text.empty()) text += " ";
    text += more;
}
} // namespace

FlowRuntime::FlowRuntime(std::shared_ptr<const CompiledAgent> agent) : _agent(std::move(agent)) {
    for (const auto& param : _agent->definition().session_params()) {
        Value initial = param.has_default_() ? param.default_() : nullValue();
        (*_session.mutable_fields())[param.name()] = initial;
    }
}

bool FlowRuntime::setSessionParam(const std::string& name, const Value& value, std::string* error) {
    const auto* param = _agent->sessionParam(name);
    if (!param) {
        if (error) *error = "no session param \"" + name + "\" is declared";
        return false;
    }
    Value stored = value;
    if (param->type() == vasdk::flow::SessionParam::STRING &&
        (value.kind_case() == Value::kNumberValue || value.kind_case() == Value::kBoolValue)) {
        stored = stringValue(toText(value));
    } else if (param->type() == vasdk::flow::SessionParam::NUMBER && value.kind_case() == Value::kStringValue) {
        char* end = nullptr;
        const double number = std::strtod(value.string_value().c_str(), &end);
        if (end != value.string_value().c_str() && *end == '\0') stored = numberValue(number);
    }
    if (!fitsType(param->type(), stored)) {
        if (error) *error = "\"" + toText(value) + "\" doesn't fit session param \"" + name + "\" (" +
                            vasdk::flow::SessionParam::Type_Name(param->type()) + ")";
        return false;
    }
    (*_session.mutable_fields())[name] = stored;
    return true;
}

Value FlowRuntime::sessionParam(const std::string& name) const {
    const auto found = _session.fields().find(name);
    return found == _session.fields().end() ? nullValue() : found->second;
}

Value FlowRuntime::lookup(const Path& path) const {
    const std::string& root = path[0];
    const std::string field = path.size() > 1 ? path[1] : "";
    auto fromFields = [&](const google::protobuf::Struct& fields, const std::string& name, size_t rest) {
        const auto found = fields.fields().find(name);
        return found == fields.fields().end() ? nullValue() : valueAt(found->second, path, rest);
    };
    if (root == "session") return fromFields(_session, field, 2);
    if (root == "page" && field == "name") return stringValue(pageName());
    if (root == "page" && field == "locals" && path.size() > 2) return fromFields(_pageLocals, path[2], 3);
    if (root == "input" && field == "text") return stringValue(_inputText);
    if (root == "nlu" && field == "intent") return _intent.empty() ? nullValue() : stringValue(_intent);
    if (root == "context" && _context) return fromFields(_context(), field, 2);
    return nullValue();
}

Value FlowRuntime::evaluate(const std::string& expression) const {
    auto compiled = _agent->expression(expression);
    if (!compiled) compiled = Expression::parse(expression);   // checked when the agent loaded
    if (!compiled) return nullValue();
    return compiled->evaluate([this](const Path& path) { return lookup(path); });
}

bool FlowRuntime::holds(const std::string& condition) const {
    return condition.empty() || isTruthy(evaluate(condition));
}

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

    _inputText = text;
    _intent.clear();
    const auto words = splitWords(text);
    const vasdk::flow::Route* best = nullptr;
    int bestScore = 0;
    for (const auto& route : _agent->page(_position).routes()) {
        if (route.intent().empty()) continue;
        const int score = _agent->matchIntent(route.intent(), words);
        if (score > bestScore && holds(route.condition())) {   // ties: the first route wins
            best = &route;
            bestScore = score;
        }
    }
    if (!best) {
        for (const auto& route : _agent->page(_position).routes()) {
            if (route.intent().empty() && holds(route.condition())) {
                best = &route;
                break;
            }
        }
    }
    if (!best) {
        reply.matched = false;
        reply.text = kNotUnderstood;
        append(reply.text, _lastQuestion);
        reply.expectsReply = !_lastQuestion.empty();
        return reply;
    }

    reply.intent = _intent = best->intent();
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
        case vasdk::flow::Step::kSet: {
            const Value value = evaluate(step.set().value());
            const Path path = splitReference(step.set().key());
            std::string error;
            if (path.size() == 2 && path[0] == "session") {
                if (!setSessionParam(path[1], value, &error)) spdlog::error("[FlowRuntime] set {}: {}", step.set().key(), error);
            } else if (path.size() == 3 && path[0] == "page" && path[1] == "locals") {
                (*_pageLocals.mutable_fields())[path[2]] = value;
            }
            break;
        }
        case vasdk::flow::Step::kBranch: {
            const auto& branch = step.branch();
            const auto outcome = run(holds(branch.if_()) ? branch.then() : branch.else_(), reply);
            if (outcome.ended || outcome.jump) return outcome;
            break;
        }
        case vasdk::flow::Step::kSwitch: {
            const auto& choice = step.switch_();
            const Value on = evaluate(choice.on());
            const google::protobuf::RepeatedPtrField<vasdk::flow::Step>* chosen = &choice.default_();
            for (const auto& option : choice.cases()) {
                if (valuesEqual(on, stringValue(option.value()))) {
                    chosen = &option.steps();
                    break;
                }
            }
            const auto outcome = run(*chosen, reply);
            if (outcome.ended || outcome.jump) return outcome;
            break;
        }
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
    const std::string text = renderTemplate(say.text(static_cast<int>(count++ % say.text_size())),
                                            [this](const Path& path) { return lookup(path); });
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
        _pageLocals.Clear();
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
