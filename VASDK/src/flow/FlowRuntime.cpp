#include "flow/FlowRuntime.h"

#include "flow/SlotExtraction.h"
#include "goal/GoalState.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include <cctype>
#include <cstdlib>
#include <utility>

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

// Adds to the reply: to its last part when spoken alike, else as a part of its own.
void append(Reply& reply, const std::string& more, bool interruptible = true, const std::string& ssml = {}) {
    if (more.empty()) return;
    if (!reply.text.empty()) reply.text += " ";
    reply.text += more;
    auto& segments = reply.segments;
    if (!segments.empty() && ssml.empty() && segments.back().ssml.empty() && segments.back().interruptible == interruptible) {
        segments.back().text += " " + more;
        return;
    }
    segments.push_back({more, ssml, interruptible});
}

// "<speak>Hi <break time='1s'/>there</speak>" → "Hi there": what a voice without SSML says.
std::string withoutTags(const std::string& ssml) {
    std::string text;
    bool inTag = false, space = false;
    for (const char c : ssml) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false, space = true;
        else if (inTag) continue;
        else if (std::isspace(static_cast<unsigned char>(c))) space = true;
        else {
            if (space && !text.empty()) text += ' ';
            space = false;
            text += c;
        }
    }
    return text;
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

bool FlowRuntime::setEntityValues(const std::string& name, std::vector<vasdk::nlu::EntityValue> values,
                                  std::string* error) {
    const auto* type = _agent->entities().type(name);
    if (!type || !type->dynamic()) {
        if (error) *error = "entity \"" + name + "\" is not a dynamic list entity";
        return false;
    }
    _dynamicEntities[name] = std::move(values);
    return true;
}

Value FlowRuntime::resolve(const Path& path, bool spoken) const {
    const std::string& root = path[0];
    const std::string field = path.size() > 1 ? path[1] : "";
    auto fromFields = [&](const google::protobuf::Struct& fields, const std::string& name, size_t rest) {
        const auto found = fields.fields().find(name);
        return found == fields.fields().end() ? nullValue() : valueAt(found->second, path, rest);
    };
    const auto& entities = _agent->entities();
    if (root == "session") return fromFields(_session, field, 2);
    if (root == "page" && field == "name") return stringValue(pageName());
    if (root == "page" && field == "locals" && path.size() > 2) return fromFields(_pageLocals, path[2], 3);
    if (root == "input" && field == "text") return stringValue(_inputText);
    if (root == "nlu" && field == "intent") return _intent.empty() ? nullValue() : stringValue(_intent);
    if (root == "nlu" && field == "entities" && path.size() > 2) {
        const auto match = entities.find(path[2], _inputText, _dynamicEntities);
        return match ? entityField(*match, path, 3, spoken) : nullValue();
    }
    if (root == "entity") return _entity ? entityField(*_entity, path, 1, spoken) : nullValue();
    if (root == "entities") {
        Value list;
        for (const auto& value : entities.spokenValues(field, _dynamicEntities)) *list.mutable_list_value()->add_values() = stringValue(value);
        return list;
    }
    if (root == "context" && _context) return fromFields(_context(), field, 2);
    if (root == "form" && _form) {
        if (field == "complete") return boolValue(_form->state->isComplete());
        if (field == "confirmed") return boolValue(_form->state->isConfirmed());
        if (field == "failed") return boolValue(_form->failed);
        const Value value = slotValue(*_form->state, field);
        const auto slot = std::find_if(_form->goal->slots().begin(), _form->goal->slots().end(),
                                       [&](const auto& candidate) { return candidate.name() == field; });
        if (slot == _form->goal->slots().end() || slot->entity().empty() || value.kind_case() == Value::kNullValue)
            return valueAt(value, path, 2);
        return entityField(entities.describe(entityName(slot->entity()), value, _dynamicEntities), path, 2, spoken);
    }
    return nullValue();
}

// Writes a value to a set or extract key, checked when the agent loaded.
void FlowRuntime::store(const std::string& key, const Value& value) {
    const Path path = splitReference(key);
    std::string error;
    if (path.size() == 2 && path[0] == "session") {
        if (!setSessionParam(path[1], value, &error)) spdlog::error("[FlowRuntime] {}: {}", key, error);
    } else if (path.size() == 3 && path[0] == "page" && path[1] == "locals") {
        (*_pageLocals.mutable_fields())[path[2]] = value;
    } else if (path.size() == 2 && path[0] == "form" && _form) {
        const auto result = _form->state->updateSlot(path[1], toText(value));
        if (!result.valid) spdlog::error("[FlowRuntime] {}: {}", key, result.errorMessage);
    }
}

// A slot's value in an answer: by its entity, else by its type.
std::optional<std::string> FlowRuntime::readSlot(const vasdk::goal::Slot& slot, const std::string& text) const {
    if (slot.entity().empty()) return extractSlotValue(slot, text);
    const auto match = _agent->entities().find(entityName(slot.entity()), text, _dynamicEntities);
    if (!match) return std::nullopt;
    return toText(match->value);
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
    _replyCut = false;
    _stack.clear();
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
        _lastAsk = nullptr;
        _stack.clear();
        _misses = 0;
        _form.reset();
        if (const auto* goal = _agent->goal(_agent->page(_position).form().goal())) {
            _form.emplace();
            _form->goal = goal;
            _form->state = _agent->newGoalState(goal->name());
        }
    }

    _inputText = chosenValue(text);
    _intent.clear();
    _entity.reset();
    const bool answeredForm = formActive() && fillForm(_inputText);
    _replyCut = false;
    if (answeredForm) {   // an answer for the form
        _misses = 0;
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
        follow(miss(kEventNoMatch, reply), reply);
        return reply;
    }
    _misses = 0;
    auto outcome = takeRoute(*best, reply);
    // Staying on the page: the form asks for what it still needs (a cleared slot, say), and
    // anything else asks its question again ("We're open 8 to 6." then the question).
    if (!outcome.moves() && formActive()) outcome = advanceForm(reply);
    else if (!outcome.moves() && !reply.expectsReply && !reply.endsSession) askAgain(reply);
    follow(std::move(outcome), reply);
    return reply;
}

Reply FlowRuntime::handleNoInput() {
    Reply reply;
    _replyCut = false;
    if (!started() || _ended) return reply;
    _inputText.clear();
    _intent.clear();
    _entity.reset();
    reply.matched = false;
    follow(miss(kEventNoInput, reply), reply);
    return reply;
}

int FlowRuntime::noInputMs() const {
    if (!started() || _ended) return 0;
    for (const auto& layer : _agent->layers(_position))
        if (layer.settings && layer.settings->has_no_input_ms()) return layer.settings->no_input_ms();
    return findHandler(kEventNoInput) ? 8000 : 0;
}

int FlowRuntime::maxReprompts() const {
    for (const auto& layer : _agent->layers(_position))
        if (layer.settings && layer.settings->has_max_reprompts()) return layer.settings->max_reprompts();
    return 2;
}

const vasdk::flow::Handler* FlowRuntime::findHandler(const std::string& event) const {
    for (const auto& layer : _agent->layers(_position))
        for (const auto& handler : *layer.handlers)
            if (handler.event() == event) return &handler;
    return nullptr;
}

// No route understood the user, or they said nothing: the handler for it, or for max-reprompts
// once too many came in a row; without one, the built-in reply and the question again.
FlowRuntime::Outcome FlowRuntime::miss(const std::string& event, Reply& reply) {
    const auto* handler = ++_misses > maxReprompts() ? findHandler(kEventMaxReprompts) : nullptr;
    reply.event = handler ? kEventMaxReprompts : event;
    if (!handler) handler = findHandler(event);
    if (handler) return runHandler(*handler, reply);
    if (event == kEventNoMatch) append(reply, kNotUnderstood);
    askAgain(reply);
    return {};
}

FlowRuntime::Outcome FlowRuntime::runHandler(const vasdk::flow::Handler& handler, Reply& reply) {
    auto outcome = run(handler.steps(), reply);
    if (!outcome.moves()) outcome = toTarget(handler.target(), "handler:" + handler.event());
    if (!outcome.moves() && !reply.expectsReply) askAgain(reply);
    return outcome;
}

// The page's question again: the form's, or the last one asked.
void FlowRuntime::askAgain(Reply& reply) {
    if (formActive()) {
        advanceForm(reply);
        return;
    }
    append(reply, _lastQuestion);
    reply.expectsReply = !_lastQuestion.empty();
    offerChoices(reply);
}

void FlowRuntime::offerChoices(Reply& reply) const {
    if (!_lastAsk) return;
    reply.choices.clear();
    for (const auto& choice : _lastAsk->choices())
        reply.choices.push_back({choice.label(), choice.value().empty() ? choice.label() : choice.value()});
}

// The value of the choice whose label, value or synonym is the whole answer; else the answer.
std::string FlowRuntime::chosenValue(const std::string& text) const {
    if (!_lastAsk || _lastAsk->choices().empty()) return text;
    const auto words = splitWords(text);
    auto said = [&words](const std::string& name) { return !name.empty() && splitWords(name) == words; };
    for (const auto& choice : _lastAsk->choices()) {
        const std::string value = choice.value().empty() ? choice.label() : choice.value();
        if (said(choice.label()) || said(value) || std::any_of(choice.synonyms().begin(), choice.synonyms().end(), said))
            return value;
    }
    return text;
}

const vasdk::flow::Route* FlowRuntime::matchRoute() {
    for (const auto& layer : _agent->layers(_position))
        if (const auto* route = matchRoute(*layer.routes)) return route;
    return nullptr;
}

const vasdk::flow::Route* FlowRuntime::matchRoute(const google::protobuf::RepeatedPtrField<vasdk::flow::Route>& routes) {
    const auto words = splitWords(_inputText);
    // A route's entity must be in the words; $entity reads it while its condition is checked.
    auto entityHolds = [this](const vasdk::flow::Route& route) {
        _entity.reset();
        if (route.entity().empty()) return true;
        _entity = _agent->entities().find(entityName(route.entity()), _inputText, _dynamicEntities);
        return _entity.has_value();
    };
    const vasdk::flow::Route* best = nullptr;
    std::optional<EntityMatch> bestEntity;
    int bestScore = 0;
    for (const auto& route : routes) {
        if (route.intent().empty()) continue;
        const int score = _agent->matchIntent(route.intent(), words);
        if (score > bestScore && entityHolds(route) && holds(route.condition())) {   // ties: the first route wins
            best = &route;
            bestEntity = _entity;
            bestScore = score;
        }
    }
    if (best) {
        _entity = bestEntity;
        return best;
    }
    for (const auto& route : routes)
        if (route.intent().empty() && !route.entity().empty() && entityHolds(route) && holds(route.condition())) return &route;
    _entity.reset();
    for (const auto& route : routes)
        if (route.intent().empty() && route.entity().empty() && holds(route.condition())) return &route;
    return nullptr;
}

FlowRuntime::Outcome FlowRuntime::toTarget(const std::string& target, std::string reason) const {
    Outcome outcome;
    if (target.empty()) return outcome;
    if (target == kTargetEndSession) {
        outcome.ended = true;
    } else if (target == kTargetEndFlow) {
        outcome.endFlow = true;
    } else {
        outcome.jump = _agent->resolve(target, _position);
        outcome.pushFlow = target.rfind("flow:", 0) == 0;
        outcome.reason = std::move(reason);
    }
    return outcome;
}

FlowRuntime::Outcome FlowRuntime::takeRoute(const vasdk::flow::Route& route, Reply& reply) {
    reply.intent = _intent = route.intent();
    auto outcome = run(route.steps(), reply);
    if (outcome.moves()) return outcome;
    return toTarget(route.target(), "route:" + route.intent());
}

void FlowRuntime::follow(Outcome outcome, Reply& reply) {
    if (outcome.ended) {
        end(reply);
    } else if (outcome.endFlow) {
        returnFromFlow(reply);
    } else if (outcome.jump) {
        if (outcome.pushFlow) pushFrame();
        moveTo(*outcome.jump, std::move(outcome.reason), reply);
    }
}

void FlowRuntime::pushFrame() {
    if (_stack.size() >= kMaxFlowDepth) {
        spdlog::error("[FlowRuntime] {} flows deep: \"{}\" won't be returned to", kMaxFlowDepth, pageName());
        return;
    }
    _stack.push_back({_position, _lastQuestion, _lastAsk, _form, _pageLocals});
}

// END_FLOW: back to the page that entered this flow, as it was, and its question again. In the
// start flow there's nowhere to go back to: the conversation ends.
void FlowRuntime::returnFromFlow(Reply& reply) {
    if (_stack.empty()) {
        end(reply);
        return;
    }
    Frame frame = std::move(_stack.back());
    _stack.pop_back();
    reply.pageChanges.push_back(changeTo(frame.page, "return"));
    _position = frame.page;
    _lastQuestion = std::move(frame.lastQuestion);
    _lastAsk = frame.lastAsk;
    _form = std::move(frame.form);
    _pageLocals = std::move(frame.pageLocals);
    _misses = 0;
    askAgain(reply);
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
            _lastAsk = &step.ask();
            reply.expectsReply = true;
            offerChoices(reply);
            break;
        case vasdk::flow::Step::kSet:
            store(step.set().key(), evaluate(step.set().value()));
            break;
        case vasdk::flow::Step::kExtract:
            if (const auto match = _agent->entities().find(entityName(step.extract().entity()), _inputText, _dynamicEntities))
                store(step.extract().into(), match->value);
            break;
        case vasdk::flow::Step::kClearSlot:
            if (_form) {
                _form->state->clearSlot(step.clear_slot().slot());
                _form->awaitingConfirmation = _form->awaitingChange = false;
            }
            break;
        case vasdk::flow::Step::kBranch: {
            const auto& branch = step.branch();
            const auto outcome = run(holds(branch.if_()) ? branch.then() : branch.else_(), reply);
            if (outcome.moves()) return outcome;
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
            if (outcome.moves()) return outcome;
            break;
        }
        case vasdk::flow::Step::kGoto:
            return toTarget(step.goto_().target(), "goto");
        case vasdk::flow::Step::kEnd: {
            Outcome outcome;
            outcome.ended = true;
            return outcome;
        }
        case vasdk::flow::Step::KIND_NOT_SET:
            break;
        }
    }
    return {};
}

std::string FlowRuntime::say(const vasdk::flow::Say& say, Reply& reply) {
    std::string text = pick(say.text(), &say);
    const std::string ssml = say.ssml().empty() ? std::string() : render(say.ssml());
    if (text.empty()) text = withoutTags(ssml);
    append(reply, text, !say.has_interruptible() || say.interruptible(), ssml);
    if (say.has_ui()) reply.ui.MergeFrom(say.ui());
    return text;
}

std::string FlowRuntime::render(const std::string& text) const {
    return renderTemplate(text, [this](const Path& path) { return resolve(path, true); });
}

std::string FlowRuntime::pick(const Texts& texts, const void* key) {
    if (texts.empty()) return {};
    auto& count = _sayCounts[key];
    return render(texts.Get(static_cast<int>(count++ % texts.size())));
}

std::string FlowRuntime::sayOne(const Texts& texts, const void* key, Reply& reply) {
    const std::string text = pick(texts, key);
    append(reply, text);
    return text;
}

std::string FlowRuntime::ask(const Texts& texts, const void* key, const std::string& fallback, Reply& reply) {
    std::string question = texts.empty() ? fallback : sayOne(texts, key, reply);
    if (texts.empty()) append(reply, fallback);
    _lastQuestion = question;
    _lastAsk = nullptr;
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
        // Only values that can't be mistaken: a list value, or a list or pattern entity.
        const auto* entity = _agent->entities().type(entityName(slot.entity()));
        const bool named = slot.entity().empty() ? slot.type() == vasdk::goal::Slot::ENUM
                                                 : entity && (entity->has_list() || entity->has_pattern());
        if (!named || slot.name() == _form->asking) continue;
        if (!overwrite && !_form->state->slotValueJson(slot.name()).is_null()) continue;
        const auto value = readSlot(slot, text);
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
        if (*answer) {
            // Cut off before its end, the read-back wasn't all heard: it's read again.
            if (!_replyCut) form.state->setConfirmed(true);
        } else if (!fillNamedValues(text, true)) {
            form.awaitingChange = true;
        }
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
                if (const auto value = readSlot(*slot, text))
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
    reply.event = kEventNoMatch;
    const int limit = form.goal->max_reprompts() > 0 ? form.goal->max_reprompts() : maxReprompts();
    if (++form.reprompts > limit && !form.failed) {
        form.failed = true;
        auto outcome = conditionRoute(reply);
        if (outcome.moves() || !reply.text.empty()) return outcome;
        if (const auto* handler = findHandler(kEventMaxReprompts)) {
            reply.event = kEventMaxReprompts;
            return runHandler(*handler, reply);
        }
    }
    const auto& goal = *form.goal;
    if (form.awaitingConfirmation) {
        append(reply, kSayYesOrNo);
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
            append(reply, kDidNotGetThat);
            ask(slot->prompt(), &slot->prompt(), "What's the " + spokenName(slot->name()) + "?", reply);
        }
    }
    return {};
}

FlowRuntime::Outcome FlowRuntime::conditionRoute(Reply& reply) {
    for (const auto& route : _agent->page(_position).routes())
        if (route.intent().empty() && route.entity().empty() && holds(route.condition())) return takeRoute(route, reply);
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
        _lastAsk = nullptr;
        _pageLocals.Clear();
        _misses = 0;
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
        if (outcome.ended || outcome.endFlow) {
            follow(std::move(outcome), reply);
            return;
        }
        if (outcome.jump && outcome.pushFlow) pushFrame();
        next = outcome.jump;
        reason = outcome.reason;
    }
}

void FlowRuntime::end(Reply& reply) {
    _ended = true;
    _stack.clear();
    reply.endsSession = true;
    reply.expectsReply = false;
}

} // namespace flow
} // namespace va
