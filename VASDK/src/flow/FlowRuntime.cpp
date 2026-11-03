#include "flow/FlowRuntime.h"

#include "flow/SlotExtraction.h"
#include "goal/GoalState.h"

#include <spdlog/spdlog.h>

#include <algorithm>

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
constexpr const char* kDidNotGetThat = "Sorry, I didn't get that.";
constexpr const char* kSayYesOrNo = "Please say yes or no.";
constexpr const char* kWhatToChange = "What would you like to change?";

// "phone_number" → "phone number": how a slot is named when spoken.
std::string spokenName(const std::string& name) {
    std::string spoken = name;
    std::replace(spoken.begin(), spoken.end(), '_', ' ');
    return spoken;
}

void append(std::string& text, const std::string& more) {
    if (more.empty()) return;
    if (!text.empty()) text += " ";
    text += more;
}
} // namespace

FlowRuntime::Form::Form() = default;
FlowRuntime::Form::Form(Form&& other) noexcept = default;
FlowRuntime::Form& FlowRuntime::Form::operator=(Form&& other) noexcept = default;
FlowRuntime::Form::~Form() = default;
FlowRuntime::Form::Form(const Form& other)
    : goal(other.goal), state(other.state ? std::make_unique<goal::GoalState>(*other.state) : nullptr),
      asking(other.asking), awaitingConfirmation(other.awaitingConfirmation), awaitingChange(other.awaitingChange),
      failed(other.failed), reprompts(other.reprompts) {}
FlowRuntime::Form& FlowRuntime::Form::operator=(const Form& other) {
    if (this != &other) *this = Form(other);
    return *this;
}

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
    if (root == "form" && _form) {
        if (field == "complete") return boolValue(_form->state->isComplete());
        if (field == "confirmed") return boolValue(_form->state->isConfirmed());
        if (field == "failed") return boolValue(_form->failed);
        return valueAt(slotValue(*_form->state, field), path, 2);
    }
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
    _inputText.clear();
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
        _form.reset();
        if (const auto* goal = _agent->goal(_agent->page(_position).form().goal())) {
            _form.emplace();
            _form->goal = goal;
            _form->state = _agent->newGoalState(goal->name());
        }
    }

    _inputText = text;
    _intent.clear();
    if (formActive() && fillForm(text)) {   // an answer for the form
        follow(advanceForm(reply), reply);
        return reply;
    }
    const auto* best = matchRoute();
    if (!best) {
        if (formActive()) {
            follow(repromptForm(reply), reply);
            return reply;
        }
        reply.matched = false;
        reply.text = kNotUnderstood;
        append(reply.text, _lastQuestion);
        reply.expectsReply = !_lastQuestion.empty();
        return reply;
    }
    auto outcome = takeRoute(*best, reply);
    // Staying on the page: the form asks for what it still needs (a cleared slot, say).
    if (!outcome.ended && !outcome.jump && formActive()) outcome = advanceForm(reply);
    follow(std::move(outcome), reply);
    return reply;
}

const vasdk::flow::Route* FlowRuntime::matchRoute() {
    const auto words = splitWords(_inputText);
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
    if (best) return best;
    for (const auto& route : _agent->page(_position).routes())
        if (route.intent().empty() && holds(route.condition())) return &route;
    return nullptr;
}

FlowRuntime::Outcome FlowRuntime::takeRoute(const vasdk::flow::Route& route, Reply& reply) {
    reply.intent = _intent = route.intent();
    auto outcome = run(route.steps(), reply);
    if (outcome.ended || route.target() == kTargetEndSession) return {std::nullopt, true};
    if (outcome.jump) return outcome;
    if (!route.target().empty()) return {_agent->resolve(route.target(), _position), false, "route:" + route.intent()};
    return {};
}

void FlowRuntime::follow(Outcome outcome, Reply& reply) {
    if (outcome.ended) end(reply);
    else if (outcome.jump) moveTo(*outcome.jump, std::move(outcome.reason), reply);
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
            } else if (path.size() == 2 && path[0] == "form" && _form) {
                const auto result = _form->state->updateSlot(path[1], toText(value));
                if (!result.valid) spdlog::error("[FlowRuntime] set {}: {}", step.set().key(), result.errorMessage);
            }
            break;
        }
        case vasdk::flow::Step::kClearSlot:
            if (_form) {
                _form->state->clearSlot(step.clear_slot().slot());
                _form->awaitingConfirmation = _form->awaitingChange = false;
            }
            break;
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
    return sayOne(say.text(), &say, reply);
}

std::string FlowRuntime::sayOne(const Texts& texts, const void* key, Reply& reply) {
    if (texts.empty()) return {};
    auto& count = _sayCounts[key];
    const std::string text = renderTemplate(texts.Get(static_cast<int>(count++ % texts.size())),
                                            [this](const Path& path) { return lookup(path); });
    append(reply.text, text);
    return text;
}

std::string FlowRuntime::ask(const Texts& texts, const void* key, const std::string& fallback, Reply& reply) {
    std::string question = texts.empty() ? fallback : sayOne(texts, key, reply);
    if (texts.empty()) append(reply.text, fallback);
    _lastQuestion = question;
    reply.expectsReply = true;
    return question;
}

//  Forms

bool FlowRuntime::formActive() const {
    if (!_form) return false;
    const bool needsYes = _form->goal->require_confirmation() && !_form->state->isConfirmed();
    return !_form->state->isComplete() || needsYes || _form->awaitingChange;
}

// Fills the ENUM slots whose values the user named ("make it tire repair"): the empty ones, or
// any with `overwrite`. True when a value changed.
bool FlowRuntime::fillNamedValues(const std::string& text, bool overwrite) {
    bool filled = false;
    for (const auto& slot : _form->goal->slots()) {
        if (slot.type() != vasdk::goal::Slot::ENUM || slot.name() == _form->asking) continue;
        if (!overwrite && !_form->state->slotValueJson(slot.name()).is_null()) continue;
        const auto value = extractSlotValue(slot, text);
        if (!value || toText(slotValue(*_form->state, slot.name())) == *value) continue;
        filled = _form->state->updateSlot(slot.name(), *value).valid || filled;
    }
    return filled;
}

bool FlowRuntime::fillForm(const std::string& text) {
    auto& form = *_form;
    bool used = false;
    if (form.awaitingConfirmation) {
        const auto answer = extractYesNo(text);
        if (!answer) return false;
        form.awaitingConfirmation = false;
        // "No, make it Tuesday": a value in the same answer changes it straight away.
        if (*answer) form.state->setConfirmed(true);
        else if (!fillNamedValues(text, true)) form.awaitingChange = true;
        used = true;
    } else if (form.awaitingChange) {
        used = fillNamedValues(text, true);
        if (!used) {
            // "The day": that slot is asked for again.
            const auto words = splitWords(text);
            for (const auto& slot : form.goal->slots()) {
                const auto name = splitWords(spokenName(slot.name()));
                if (std::search(words.begin(), words.end(), name.begin(), name.end()) != words.end()) {
                    form.state->clearSlot(slot.name());
                    used = true;
                    break;
                }
            }
        }
        if (used) form.awaitingChange = false;
    } else {
        if (!form.asking.empty()) {
            const auto slot = std::find_if(form.goal->slots().begin(), form.goal->slots().end(),
                                           [&](const auto& candidate) { return candidate.name() == form.asking; });
            if (slot != form.goal->slots().end())
                if (const auto value = extractSlotValue(*slot, text))
                    used = form.state->updateSlot(form.asking, *value).valid;
        }
        used = fillNamedValues(text, false) || used;
    }
    if (used) form.reprompts = 0;
    return used;
}

// Asks for what the form still needs: the next slot, the read-back, or what to change. Once it
// has everything, the first condition route that holds is taken.
FlowRuntime::Outcome FlowRuntime::advanceForm(Reply& reply) {
    auto& form = *_form;
    form.asking.clear();
    const auto& goal = *form.goal;
    if (form.awaitingChange) {
        ask(goal.change(), &goal.change(), kWhatToChange, reply);
        return {};
    }
    const auto missing = form.state->missingSlots();
    if (!missing.empty()) {
        form.asking = missing.front();
        const auto& slot = *std::find_if(goal.slots().begin(), goal.slots().end(),
                                         [&](const auto& candidate) { return candidate.name() == form.asking; });
        ask(slot.prompt(), &slot.prompt(), "What's the " + spokenName(slot.name()) + "?", reply);
        return {};
    }
    if (goal.require_confirmation() && !form.state->isConfirmed()) {
        form.awaitingConfirmation = true;
        ask(goal.confirm(), &goal.confirm(), "", reply);
        return {};
    }
    return conditionRoute(reply);
}

FlowRuntime::Outcome FlowRuntime::repromptForm(Reply& reply) {
    auto& form = *_form;
    reply.matched = false;
    const int maxReprompts = form.goal->max_reprompts() > 0 ? form.goal->max_reprompts() : 2;
    if (++form.reprompts > maxReprompts && !form.failed) {
        form.failed = true;
        auto outcome = conditionRoute(reply);
        if (outcome.ended || outcome.jump || !reply.text.empty()) return outcome;
    }
    const auto& goal = *form.goal;
    if (form.awaitingConfirmation) {
        append(reply.text, kSayYesOrNo);
        ask(goal.confirm(), &goal.confirm(), "", reply);
    } else if (form.awaitingChange) {
        ask(goal.change(), &goal.change(), kWhatToChange, reply);
    } else {
        const auto slot = std::find_if(goal.slots().begin(), goal.slots().end(),
                                       [&](const auto& candidate) { return candidate.name() == form.asking; });
        if (slot == goal.slots().end()) return advanceForm(reply);
        if (!slot->reprompt().empty()) {
            ask(slot->reprompt(), &slot->reprompt(), "", reply);
        } else {
            append(reply.text, kDidNotGetThat);
            ask(slot->prompt(), &slot->prompt(), "What's the " + spokenName(slot->name()) + "?", reply);
        }
    }
    return {};
}

FlowRuntime::Outcome FlowRuntime::conditionRoute(Reply& reply) {
    for (const auto& route : _agent->page(_position).routes())
        if (route.intent().empty() && holds(route.condition())) return takeRoute(route, reply);
    return {};
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
        const bool samePage = _position == *next;
        _position = *next;
        _lastQuestion.clear();
        _pageLocals.Clear();
        const auto* goal = _agent->goal(_agent->page(_position).form().goal());
        if (!goal) {
            _form.reset();
        } else if (!samePage || !_form) {   // a form starts empty on arrival, and keeps its values on CURRENT_PAGE
            _form.emplace();
            _form->goal = goal;
            _form->state = _agent->newGoalState(goal->name());
        }
        auto outcome = run(_agent->page(_position).on_enter(), reply);
        if (!outcome.ended && !outcome.jump && _form) {
            if (!samePage && !_inputText.empty()) fillNamedValues(_inputText, false);   // "book an oil change"
            outcome = advanceForm(reply);
        }
        if (outcome.ended) {
            end(reply);
            return;
        }
        next = outcome.jump;
        reason = outcome.reason;
    }
}

void FlowRuntime::end(Reply& reply) {
    _ended = true;
    reply.endsSession = true;
    reply.expectsReply = false;
}

} // namespace flow
} // namespace va
