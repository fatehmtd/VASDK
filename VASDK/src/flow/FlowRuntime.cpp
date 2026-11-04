#include "flow/FlowRuntime.h"

#include "flow/SlotExtraction.h"
#include "goal/GoalState.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#include <algorithm>

#include <cctype>
#include <cstdlib>
#include <ctime>
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
constexpr const char* kToolFailed = "Sorry, I can't do that right now.";

nlohmann::json toJson(const Value& value) {
    std::string json;
    if (!google::protobuf::util::MessageToJsonString(value, &json).ok()) return nullptr;
    return nlohmann::json::parse(json, nullptr, false);
}

Value fromJson(const nlohmann::json& json) {
    Value value;
    if (!google::protobuf::util::JsonStringToMessage(json.dump(), &value).ok()) return nullValue();
    return value;
}
constexpr const char* kDidNotGetThat = "Sorry, I didn't get that.";
constexpr const char* kSayYesOrNo = "Please say yes or no.";
constexpr const char* kWhatToChange = "What would you like to change?";
constexpr size_t kMaxSlotChoices = 5;   // a slot with more values offers no buttons

using Expectation = vasdk::flow::Expectation;

// The derived expectation, with the fields `given` sets in place of its own.
Expectation merged(Expectation derived, const Expectation& given) {
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    given.GetReflection()->ListFields(given, &fields);
    for (const auto* field : fields)
        if (field->is_repeated()) derived.GetReflection()->ClearField(&derived, field);
    derived.MergeFrom(given);
    return derived;
}

// What a form slot's answer is, from its type, then its own expect.
Expectation slotExpectation(const vasdk::goal::Slot& slot) {
    using Slot = vasdk::goal::Slot;
    Expectation expectation;
    expectation.set_mode(Expectation::SLOT);
    expectation.set_slot(slot.name());
    if (slot.entity().empty()) {
        if (slot.type() == Slot::PHONE) {
            expectation.set_mode(Expectation::DIGITS);
            expectation.set_min_digits(7);   // as GoalState accepts: 7 or 10, or 11 starting with 1
            expectation.set_max_digits(11);
        }
        if (slot.type() == Slot::PHONE || slot.type() == Slot::INTEGER || slot.type() == Slot::NUMBER)
            expectation.set_keyboard(Expectation::NUMERIC);
        if (slot.type() == Slot::EMAIL) expectation.set_keyboard(Expectation::EMAIL);
        if (slot.type() == Slot::BOOLEAN) expectation.set_mode(Expectation::YES_NO);
    }
    if (slot.sensitive()) expectation.set_secure_entry(true);
    return merged(std::move(expectation), slot.expect());
}

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
// "type.googleapis.com/acme.Points" → "acme.Points".
std::string typeName(const google::protobuf::Any& any) {
    const auto slash = any.type_url().rfind('/');
    return slash == std::string::npos ? any.type_url() : any.type_url().substr(slash + 1);
}

// A page's params for its C++ page type; an empty message when it takes none.
const google::protobuf::Message& paramsOf(const CompiledAgent::PageExtensions& extensions) {
    return extensions.params ? *extensions.params : vasdk::flow::End::default_instance();
}
} // namespace

//  TurnContext: what C++ steps, page types and envelopes see of the conversation

Value TurnContext::lookup(const std::string& reference) const {
    const auto expression = Expression::parse(reference);
    const Path* path = expression ? expression->reference() : nullptr;
    return path ? _runtime.lookup(*path) : nullValue();
}
void TurnContext::set(const std::string& key, const Value& value) { _runtime.store(key, value); }
const std::string& TurnContext::text() const { return _runtime._inputText; }
void TurnContext::setText(std::string text) {
    _runtime._inputText = std::move(text);
    _runtime._understood.reset();
}
void TurnContext::say(const std::string& text) { append(_reply, text); }
std::string TurnContext::render(const std::string& text) const { return _runtime.render(text); }
void TurnContext::goTo(const std::string& target) { _target = target; }
const std::string& TurnContext::replyText() const { return _reply.text; }

bool TurnContext::apply(const vasdk::flow::StepOutcome& outcome, std::string* error) {
    const auto session = _runtime._session;
    const auto pageLocals = _runtime._pageLocals;
    const auto form = _runtime._form;
    auto refuse = [&](const std::string& why) {
        _runtime._session = session;
        _runtime._pageLocals = pageLocals;
        _runtime._form = form;
        spdlog::error("[FlowRuntime] step outcome refused on page \"{}\": {}", _runtime.pageName(), why);
        if (error) *error = why;
        return false;
    };
    std::string problem;
    for (const auto& [key, value] : outcome.set())
        if (!_runtime.store(key, value, &problem)) return refuse(problem);
    for (const auto& key : outcome.unset())
        if (!_runtime.store(key, nullValue(), &problem)) return refuse(problem);
    if (!outcome.goto_().empty() && !_runtime.toTarget(outcome.goto_(), "goto").moves())
        return refuse("unknown target \"" + outcome.goto_() + "\"");
    for (const auto& text : outcome.say()) append(_reply, text);
    if (!outcome.goto_().empty()) _target = outcome.goto_();
    return true;
}
std::string TurnContext::flowName() const { return _runtime.flowName(); }
std::string TurnContext::pageName() const { return _runtime.pageName(); }

void TurnContext::ask(const std::string& question, const std::vector<vasdk::flow::Choice>& choices,
                      const vasdk::flow::Expectation& expect) {
    append(_reply, question);
    _runtime._lastQuestion = question;
    _runtime._questionChoices = choices;
    Expectation derived;
    derived.set_mode(choices.empty() ? Expectation::OPEN : Expectation::CHOICE);
    _runtime._expectation = merged(std::move(derived), expect);
    _reply.expectsReply = true;
    _runtime.offerChoices(_reply);
}

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
    if (root == "input" && field == "digit_count") return numberValue(static_cast<double>(digitCount(_inputText)));
    if (root == "nlu" && field == "intent") return _intent.empty() ? nullValue() : stringValue(_intent);
    if (root == "nlu" && (field == "confidence" || field == "stage" || field == "alternatives")) {
        if (!_understood) return nullValue();
        if (field == "stage") return stringValue(_understood->stage);
        if (field == "alternatives") {
            Value list;
            for (const auto& intent : _alternatives) *list.mutable_list_value()->add_values() = stringValue(intent);
            return list;
        }
        for (const auto& match : _understood->intents)
            if (match.intent == _intent) return numberValue(match.confidence);
        return nullValue();
    }
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
    if (root == "trigger" && _trigger) {
        if (field == "name") return stringValue(_trigger->first);
        Value payload;
        *payload.mutable_struct_value() = _trigger->second;
        if (field == "payload") return valueAt(payload, path, 2);
    }
    if (root == "params" && started()) return valueAt(_agent->extensions(_position).paramsValue, path, 1);
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

// Writes a value to a set, extract or call_tool key (checked when the agent loaded), or a key
// from C++ (checked here). A null value empties it.
bool FlowRuntime::store(const std::string& key, const Value& value, std::string* error) {
    const Path path = splitReference(key);
    std::string problem;
    if (path.size() == 2 && path[0] == "session") {
        setSessionParam(path[1], value, &problem);
    } else if (path.size() == 3 && path[0] == "page" && path[1] == "locals") {
        if (value.kind_case() == Value::kNullValue) _pageLocals.mutable_fields()->erase(path[2]);
        else (*_pageLocals.mutable_fields())[path[2]] = value;
    } else if (path.size() == 2 && path[0] == "form" && _form &&
               std::any_of(_form->goal->slots().begin(), _form->goal->slots().end(),
                           [&](const auto& slot) { return slot.name() == path[1]; })) {
        if (value.kind_case() == Value::kNullValue) {
            _form->state->clearSlot(path[1]);
        } else if (const auto result = _form->state->updateSlot(path[1], toText(value)); !result.valid) {
            problem = result.errorMessage;
        }
    } else {
        problem = "can't be written: use $session.<declared param>, $page.locals.<name> or, on a form page, $form.<slot>";
    }
    if (problem.empty()) return true;
    if (error) *error = key + ": " + problem;
    else spdlog::error("[FlowRuntime] {}: {}", key, problem);
    return false;
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
    _offer.reset();
    // A session-start trigger whose condition holds takes the place of the start page's entry.
    const vasdk::flow::Trigger* opening = nullptr;
    for (const auto& trigger : _agent->definition().triggers()) {
        if (!trigger.source().has_session_start()) continue;
        _trigger.emplace(trigger.name(), google::protobuf::Struct());
        if (holds(trigger.condition())) {
            opening = &trigger;
            break;
        }
    }
    if (!opening) {
        _trigger.reset();
        moveTo(_agent->startPage(), "start", reply);
    } else {
        reply.triggers.push_back({opening->name(), vasdk::flow::STARTED, ""});
        const auto& action = opening->action();
        if (action.has_start_flow()) {   // into the flow; coming back enters the start page
            enterStartQuietly(reply, "start");
            pushFrame();
            if (!_stack.empty()) _stack.back().reenter = true;
            follow(toTarget("flow:" + action.start_flow(), "trigger:" + opening->name()), reply);
        } else if (action.has_goto_()) {
            const auto target = _agent->resolve(action.goto_(), _agent->startPage());
            moveTo(target ? *target : _agent->startPage(), "trigger:" + opening->name(), reply);
        } else {
            sayOne(action.say().text(), &action.say(), reply);
            moveTo(_agent->startPage(), "start", reply);
        }
    }
    runAfterEnvelopes(reply);
    finish(reply);
    return reply;
}

Reply FlowRuntime::handleText(const std::string& text) {
    Reply reply;
    if (!_offer || !answerOffer(text, reply)) takeTurn(text, reply);
    runAfterEnvelopes(reply);
    deliverDue(reply);   // a queued trigger whose moment has come, after the reply
    finish(reply);
    return reply;
}

Reply FlowRuntime::selectChoice(int index) {
    if (index < 0 || static_cast<size_t>(index) >= _choices.size()) {
        spdlog::warn("[FlowRuntime] no choice {} to select: the last reply offered {}", index, _choices.size());
        Reply reply;
        reply.matched = false;
        return reply;
    }
    return handleText(_choices[static_cast<size_t>(index)].value);
}

// What the reply expects, and how long to listen after it: a say's own value, the question's,
// else the page's settings, its envelopes', the flow's and the agent's.
void FlowRuntime::finish(Reply& reply) {
    reply.sensitiveValues = sensitiveValues();
    if (reply.expectsReply) {
        reply.expectation = _expectation;
        if (reply.expectation.mode() == Expectation::MODE_UNSET) reply.expectation.set_mode(Expectation::OPEN);
        _choices = reply.choices;
    } else {
        reply.expectation.set_mode(Expectation::NONE);
        _choices.clear();
    }
    if (reply.followUpListenMs || !started()) return;
    if (reply.expectsReply && reply.expectation.has_follow_up_listen_ms()) {
        reply.followUpListenMs = reply.expectation.follow_up_listen_ms();
        return;
    }
    for (const auto& layer : _agent->layers(_position)) {
        if (layer.settings && layer.settings->has_follow_up_listen_ms()) {
            reply.followUpListenMs = layer.settings->follow_up_listen_ms();
            return;
        }
    }
}

void FlowRuntime::takeTurn(const std::string& text, Reply& reply) {
    if (!started() || _ended) enterStartQuietly(reply, started() ? "restart" : "start");

    _inputText = text;
    _intent.clear();
    _entity.reset();
    _understood.reset();
    _alternatives.clear();
    // Envelopes written in C++, outermost first, then a page type written in C++: each may
    // answer the turn itself.
    const auto& extensions = _agent->extensions(_position);
    for (auto envelope = extensions.envelopes.rbegin(); envelope != extensions.envelopes.rend(); ++envelope) {
        TurnContext turn(*this, reply);
        if (!(*envelope)->before || !(*envelope)->before(turn)) continue;
        _replyCut = false;
        _misses = 0;
        follow(answered(fromContext(turn), reply), reply);
        return;
    }
    _inputText = chosenValue(_inputText);
    if (const auto* type = extensions.registeredType; type && type->onTurn) {
        TurnContext turn(*this, reply);
        if (type->onTurn(paramsOf(extensions), turn)) {
            _replyCut = false;
            _misses = 0;
            follow(answered(fromContext(turn), reply), reply);
            return;
        }
    }
    const bool answeredForm = formActive() && fillForm(_inputText);
    _replyCut = false;
    if (answeredForm) {   // an answer for the form
        _misses = 0;
        follow(advanceForm(reply), reply);
        return;
    }
    const auto* best = matchRoute();
    if (best && _alternatives.size() > 1) {   // as likely one as another: ask which, when the agent says how
        if (const auto* clarify = findHandler(kEventClarify)) {
            reply.event = kEventClarify;
            follow(runHandler(*clarify, reply), reply);
            return;
        }
    }
    if (!best) {
        if (formActive()) {
            follow(repromptForm(reply), reply);
            return;
        }
        reply.matched = false;
        follow(miss(kEventNoMatch, reply), reply);
        return;
    }
    _misses = 0;
    follow(answered(takeRoute(*best, reply), reply), reply);
}

// Staying on the page after the turn was answered: the form asks for what it still needs (a
// cleared slot, say), and anything else asks its question again ("We're open 8 to 6." then the question).
FlowRuntime::Outcome FlowRuntime::answered(Outcome outcome, Reply& reply) {
    if (!outcome.moves() && formActive()) return advanceForm(reply);
    if (!outcome.moves() && !reply.expectsReply && !reply.endsSession) askAgain(reply);
    return outcome;
}

FlowRuntime::Outcome FlowRuntime::fromContext(const TurnContext& turn) const {
    if (turn._target.empty()) return {};
    auto outcome = toTarget(turn._target, "goto");
    if (!outcome.moves()) spdlog::error("[FlowRuntime] goTo: unknown target \"{}\" from page \"{}\"", turn._target, pageName());
    return outcome;
}

// Envelopes written in C++, innermost first, once the reply is ready.
void FlowRuntime::runAfterEnvelopes(Reply& reply) {
    if (!started()) return;
    for (const auto* envelope : _agent->extensions(_position).envelopes) {
        if (!envelope->after) continue;
        TurnContext turn(*this, reply);
        envelope->after(turn);
    }
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
    runAfterEnvelopes(reply);
    deliverDue(reply);
    finish(reply);
    return reply;
}

int FlowRuntime::noInputMs() const {
    if (!started() || _ended) return 0;
    if (_expectation.has_no_input_ms()) return _expectation.no_input_ms();
    for (const auto& layer : _agent->layers(_position))
        if (layer.settings && layer.settings->has_no_input_ms()) return layer.settings->no_input_ms();
    return findHandler(kEventNoInput) ? 8000 : 0;
}

int FlowRuntime::maxReprompts() const {
    if (_expectation.has_max_reprompts()) return _expectation.max_reprompts();
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
    reply.choices.clear();
    for (const auto& choice : _questionChoices)
        reply.choices.push_back({choice.label(), choice.value().empty() ? choice.label() : choice.value()});
}

// The value of the choice whose label, value or synonym is the whole answer; else the answer.
std::string FlowRuntime::chosenValue(const std::string& text) const {
    if (_questionChoices.empty()) return text;
    const auto words = splitWords(text);
    auto said = [&words](const std::string& name) { return !name.empty() && splitWords(name) == words; };
    for (const auto& choice : _questionChoices) {
        const std::string value = choice.value().empty() ? choice.label() : choice.value();
        if (said(choice.label()) || said(value) || std::any_of(choice.synonyms().begin(), choice.synonyms().end(), said))
            return value;
    }
    return text;
}

// The question's expect narrows what's matched: only its intents on the page, and no envelope
// routes without allow_global.
const vasdk::flow::Route* FlowRuntime::matchRoute() {
    const bool global = !_expectation.has_allow_global() || _expectation.allow_global();
    for (const auto& layer : _agent->layers(_position)) {
        const bool own = layer.settings == nullptr;
        if (!own && !global) continue;
        if (const auto* route = matchRoute(*layer.routes, own ? &_expectation.intents() : nullptr)) return route;
    }
    return nullptr;
}

const vasdk::flow::Route* FlowRuntime::matchRoute(const google::protobuf::RepeatedPtrField<vasdk::flow::Route>& routes,
                                                  const google::protobuf::RepeatedPtrField<std::string>* intents) {
    // A route's entity must be in the words; $entity reads it while its condition is checked.
    auto entityHolds = [this](const vasdk::flow::Route& route) {
        _entity.reset();
        if (route.entity().empty()) return true;
        _entity = _agent->entities().find(entityName(route.entity()), _inputText, _dynamicEntities);
        return _entity.has_value();
    };
    const auto& understood = understanding();
    auto scoreOf = [&](const std::string& intent) {
        for (const auto& match : understood.intents)
            if (match.intent == intent) return match.score;
        return 0.0f;
    };
    const vasdk::flow::Route* best = nullptr;
    std::optional<EntityMatch> bestEntity;
    float bestScore = 0;
    std::vector<std::string> tied;   // intents as good as the best: the first route wins, or clarify
    for (const auto& route : routes) {
        if (route.intent().empty()) continue;
        if (intents && !intents->empty() && std::find(intents->begin(), intents->end(), route.intent()) == intents->end()) continue;
        const float score = scoreOf(route.intent());
        if (score <= 0 || score < bestScore) continue;
        if (score > bestScore && entityHolds(route) && holds(route.condition())) {
            best = &route;
            bestEntity = _entity;
            bestScore = score;
            tied = {route.intent()};
        } else if (score == bestScore && best && route.intent() != best->intent() &&
                   std::find(tied.begin(), tied.end(), route.intent()) == tied.end() && entityHolds(route) && holds(route.condition())) {
            tied.push_back(route.intent());
        }
    }
    if (best) {
        _entity = bestEntity;
        _alternatives = std::move(tied);
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
    _stack.push_back({_position, _lastQuestion, _questionChoices, _expectation, _form, _pageLocals});
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
    if (frame.reenter) {   // entered from the start, before the start page ran
        moveTo(frame.page, "return", reply);
        return;
    }
    reply.pageChanges.push_back(changeTo(frame.page, "return"));
    _position = frame.page;
    _lastQuestion = std::move(frame.lastQuestion);
    _questionChoices = std::move(frame.questionChoices);
    _expectation = std::move(frame.expectation);
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
        case vasdk::flow::Step::kAsk: {
            _lastQuestion = say(step.ask().say(), reply);
            _questionChoices.assign(step.ask().choices().begin(), step.ask().choices().end());
            Expectation derived;
            derived.set_mode(step.ask().choices().empty() ? Expectation::OPEN : Expectation::CHOICE);
            _expectation = merged(std::move(derived), step.ask().expect());
            reply.expectsReply = true;
            offerChoices(reply);
            break;
        }
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
        case vasdk::flow::Step::kCallTool:
            if (auto outcome = callTool(step.call_tool(), reply); outcome.moves()) return outcome;
            break;
        case vasdk::flow::Step::kCustom: {
            const auto* function = _agent->registry().step(typeName(step.custom()));
            const auto* params = _agent->params(step.custom());
            if (!function || !params) break;   // checked when the agent loaded
            TurnContext turn(*this, reply);
            (*function)(*params, turn);
            if (auto outcome = fromContext(turn); outcome.moves()) return outcome;
            break;
        }
        case vasdk::flow::Step::KIND_NOT_SET:
            break;
        }
    }
    return {};
}

FlowRuntime::Outcome FlowRuntime::callTool(const vasdk::flow::CallTool& call, Reply& reply) {
    if (!call.filler().empty()) {
        sayOne(call.filler(), &call, reply);
        if (_fillerSink) {   // heard now, while the tool runs
            _fillerSink(reply);
            reply.text.clear();
            reply.segments.clear();
        }
    }
    nlohmann::json args = nlohmann::json::object();
    for (const auto& [name, expression] : call.args()) args[name] = toJson(evaluate(expression));
    if (!_toolCaller) _toolCaller = ToolRunner::caller(std::make_shared<ToolRunner>(_agent));
    const auto result = _toolCaller(call.name(), args);
    ReplyToolCall record;
    record.name = call.name();
    record.argumentsJson = args.dump();
    record.ok = result.ok;
    record.resultJson = result.ok ? result.value.dump() : nlohmann::json{{"error", result.error}}.dump();
    reply.toolCalls.push_back(std::move(record));
    if (result.ok) {
        if (!call.into().empty()) store(call.into(), fromJson(result.value));
        return {};
    }
    spdlog::warn("[FlowRuntime] tool \"{}\" failed on page \"{}\": {}", call.name(), pageName(), result.error);
    reply.event = kEventToolError;
    Outcome outcome;
    if (const auto* handler = findHandler(kEventToolError)) {
        outcome = runHandler(*handler, reply);
    } else {
        append(reply, kToolFailed);
        askAgain(reply);
    }
    outcome.halted = true;
    return outcome;
}

// What the words mean, worked out once per turn: the agent's rules, or the NLU set with setNlu.
const nlu::NluResult& FlowRuntime::understanding() {
    if (!_understood) {
        nlu::NluRequest request;
        request.text = _inputText;
        request.mentions = [this](const std::string& entity) {
            return _agent->entities().find(entity, _inputText, _dynamicEntities).has_value();
        };
        const std::shared_ptr<const nlu::NluProvider> provider = _nlu ? _nlu : _agent->rules();
        _understood = provider->understand(request);
    }
    return *_understood;
}

//  Triggers

FlowRuntime::TimePoint FlowRuntime::now() const { return _clock ? _clock() : std::chrono::system_clock::now(); }

// The start page, without running it: a turn before start(), or a trigger with no conversation.
void FlowRuntime::enterStartQuietly(Reply& reply, const std::string& reason) {
    reply.pageChanges.push_back(changeTo(_agent->startPage(), reason));
    _position = _agent->startPage();
    _ended = false;
    _lastQuestion.clear();
    _questionChoices.clear();
    _expectation.Clear();
    _stack.clear();
    _misses = 0;
    _form.reset();
    if (const auto* goal = _agent->goal(_agent->page(_position).form().goal())) {
        _form.emplace();
        _form->goal = goal;
        _form->state = _agent->newGoalState(goal->name());
    }
}

bool FlowRuntime::quietHours(TimePoint when) const {
    const auto& policy = _agent->definition().proactive_policy();
    if (policy.quiet_from().empty() || policy.quiet_until().empty()) return false;
    auto minuteOf = [](const std::string& time) { return std::stoi(time.substr(0, 2)) * 60 + std::stoi(time.substr(3, 2)); };
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const int minute = local.tm_hour * 60 + local.tm_min;
    const int from = minuteOf(policy.quiet_from()), until = minuteOf(policy.quiet_until());
    return from <= until ? minute >= from && minute < until : minute >= from || minute < until;   // past midnight
}

Reply FlowRuntime::fireTrigger(const std::string& name, const google::protobuf::Struct& payload, bool betweenTurns) {
    Reply reply;
    bool known = false;
    for (const auto& trigger : _agent->definition().triggers()) {
        if (trigger.source().has_session_start()) continue;
        const auto& event = trigger.source().app_event().name();
        if (trigger.name() != name && !(trigger.source().has_app_event() && event == name)) continue;
        known = true;
        arbitrate(trigger, payload, betweenTurns, reply);
    }
    if (!known) {
        spdlog::warn("[FlowRuntime] no trigger or app event \"{}\"", name);
        reply.triggers.push_back({name, vasdk::flow::DROPPED, "no such trigger"});
    }
    if (!reply.text.empty() || reply.expectsReply) finish(reply);   // else the last reply's choices stay
    return reply;
}

void FlowRuntime::arbitrate(const vasdk::flow::Trigger& trigger, const google::protobuf::Struct& payload, bool betweenTurns,
                            Reply& reply) {
    using Delivery = vasdk::flow::Delivery;
    const auto& delivery = trigger.delivery();
    auto decide = [&](vasdk::flow::TriggerDecision decision, std::string reason) {
        if (!reason.empty()) spdlog::info("[FlowRuntime] trigger \"{}\": {}", trigger.name(), reason);
        reply.triggers.push_back({trigger.name(), decision, std::move(reason)});
    };
    const auto when = now();
    const auto saved = _trigger;
    _trigger.emplace(trigger.name(), payload);
    const bool holdsNow = holds(trigger.condition());
    const std::string dedupe = delivery.dedupe_key().empty() ? std::string() : toText(evaluate(delivery.dedupe_key()));
    _trigger = saved;
    if (!holdsNow) return decide(vasdk::flow::DROPPED, "its condition doesn't hold");
    if (trigger.source().has_inactivity() && !conversationActive()) return decide(vasdk::flow::DROPPED, "no conversation");
    if (delivery.priority() != Delivery::CRITICAL && quietHours(when)) return decide(vasdk::flow::SUPPRESSED, "quiet hours");
    if (const auto last = _lastDelivered.find(trigger.name());
        last != _lastDelivered.end() && when - last->second < std::chrono::milliseconds(delivery.cooldown_ms()))
        return decide(vasdk::flow::DROPPED, "cooling down");
    while (!_deliveredThisHour.empty() && when - _deliveredThisHour.front() >= std::chrono::hours(1)) _deliveredThisHour.pop_front();
    if (const int most = _agent->definition().proactive_policy().max_per_hour();
        most > 0 && static_cast<int>(_deliveredThisHour.size()) >= most)
        return decide(vasdk::flow::DROPPED, "max_per_hour reached");
    for (const auto& queued : _queuedTriggers)
        if (!dedupe.empty() && queued.trigger == &trigger && queued.dedupe == dedupe)
            return decide(vasdk::flow::DROPPED, "already queued for " + dedupe);

    QueuedTrigger fired{&trigger, payload, when, dedupe};
    if (!conversationActive()) return deliver(fired, reply);   // no conversation to interrupt
    switch (delivery.interrupt()) {
    case Delivery::NEVER:
        return decide(vasdk::flow::DROPPED, "a conversation is going on");
    case Delivery::IMMEDIATE:
        return deliver(fired, reply);
    case Delivery::AFTER_FLOW:
        if (betweenTurns && !formActive() && _stack.empty() && !_offer) return deliver(fired, reply);
        break;
    default:   // AFTER_TURN
        if (betweenTurns && !_offer) return deliver(fired, reply);
        break;
    }
    _queuedTriggers.push_back(std::move(fired));
    decide(vasdk::flow::QUEUED, "");
}

// Says it (or asks first), then the conversation's question again when nothing else asked one.
void FlowRuntime::deliver(const QueuedTrigger& queued, Reply& reply) {
    const auto& trigger = *queued.trigger;
    const auto when = now();
    _lastDelivered[trigger.name()] = when;
    _deliveredThisHour.push_back(when);
    reply.triggers.push_back({trigger.name(), vasdk::flow::STARTED, ""});
    if (trigger.delivery().has_follow_up_listen_ms()) reply.followUpListenMs = trigger.delivery().follow_up_listen_ms();
    _trigger.emplace(trigger.name(), queued.payload);
    if (!conversationActive()) enterStartQuietly(reply, "trigger:" + trigger.name());
    if (trigger.delivery().has_ask_first()) {
        _offer = Offer{queued, _lastQuestion, _questionChoices, _expectation};
        const auto& question = trigger.delivery().ask_first();
        _lastQuestion = sayOne(question.text(), &question, reply);
        _questionChoices.clear();
        _expectation.Clear();
        _expectation.set_mode(Expectation::YES_NO);
        reply.choices.clear();
        reply.expectsReply = true;
        return;
    }
    runTriggerAction(trigger, reply);
}

void FlowRuntime::runTriggerAction(const vasdk::flow::Trigger& trigger, Reply& reply) {
    const auto& action = trigger.action();
    if (action.has_start_flow()) {
        follow(toTarget("flow:" + action.start_flow(), "trigger:" + trigger.name()), reply);
    } else if (action.has_goto_()) {
        follow(toTarget(action.goto_(), "trigger:" + trigger.name()), reply);
    } else {
        const bool asked = reply.expectsReply;   // appended to a reply that asks: it needn't be asked again
        sayOne(action.say().text(), &action.say(), reply);
        if (!asked && !_ended) askAgain(reply);
    }
}

// The queued trigger whose moment has come, highest priority first; expired ones are dropped.
bool FlowRuntime::deliverDue(Reply& reply) {
    using Delivery = vasdk::flow::Delivery;
    const auto when = now();
    for (auto queued = _queuedTriggers.begin(); queued != _queuedTriggers.end();) {
        const int expiresMs = queued->trigger->delivery().expires_after_ms();
        if (expiresMs > 0 && when - queued->firedAt >= std::chrono::milliseconds(expiresMs)) {
            reply.triggers.push_back({queued->trigger->name(), vasdk::flow::EXPIRED, "queued past expires_after_ms"});
            queued = _queuedTriggers.erase(queued);
        } else {
            ++queued;
        }
    }
    if (_offer || _ended) return false;
    auto rank = [](const QueuedTrigger& queued) {
        switch (queued.trigger->delivery().priority()) {
        case Delivery::CRITICAL: return 3;
        case Delivery::HIGH: return 2;
        case Delivery::LOW: return 0;
        default: return 1;
        }
    };
    auto best = _queuedTriggers.end();
    for (auto queued = _queuedTriggers.begin(); queued != _queuedTriggers.end(); ++queued) {
        const bool due = queued->trigger->delivery().interrupt() != Delivery::AFTER_FLOW || (!formActive() && _stack.empty());
        if (due && (best == _queuedTriggers.end() || rank(*queued) > rank(*best))) best = queued;
    }
    if (best == _queuedTriggers.end()) return false;
    const QueuedTrigger due = std::move(*best);
    _queuedTriggers.erase(best);
    deliver(due, reply);
    return true;
}

Reply FlowRuntime::deliverQueued() {
    Reply reply;
    if (deliverDue(reply)) finish(reply);
    return reply;
}

// The answer to a trigger's ask_first: yes runs its action, no says so; anything else lets the
// offer lapse and is a turn as usual (false).
bool FlowRuntime::answerOffer(const std::string& text, Reply& reply) {
    Offer offer = std::move(*_offer);
    _offer.reset();
    _lastQuestion = std::move(offer.lastQuestion);
    _questionChoices = std::move(offer.questionChoices);
    _expectation = std::move(offer.expectation);
    const auto answer = extractYesNo(text);
    if (!answer) return false;
    if (*answer) {
        runTriggerAction(*offer.trigger.trigger, reply);
    } else {
        append(reply, "Okay.");
        askAgain(reply);
    }
    return true;
}

std::vector<std::pair<std::string, int>> FlowRuntime::inactivityTriggers() const {
    std::vector<std::pair<std::string, int>> found;
    for (const auto& trigger : _agent->definition().triggers())
        if (trigger.source().has_inactivity()) found.emplace_back(trigger.name(), trigger.source().inactivity().after_ms());
    return found;
}

std::vector<std::string> FlowRuntime::sensitiveValues() const {
    std::vector<std::string> values;
    auto add = [&](const Path& path) {
        for (const bool spoken : {false, true}) {
            const Value value = resolve(path, spoken);
            if (value.kind_case() == Value::kStringValue || value.kind_case() == Value::kNumberValue) values.push_back(toText(value));
        }
    };
    for (const auto& param : _agent->definition().session_params())
        if (param.sensitive()) add({"session", param.name()});
    if (_form)
        for (const auto& slot : _form->goal->slots())
            if (slot.sensitive()) add({"form", slot.name()});
    return values;
}

std::string FlowRuntime::say(const vasdk::flow::Say& say, Reply& reply) {
    std::string text = pick(say.text(), &say);
    const std::string ssml = say.ssml().empty() ? std::string() : render(say.ssml());
    if (text.empty()) text = withoutTags(ssml);
    append(reply, text, !say.has_interruptible() || say.interruptible(), ssml);
    if (say.has_ui()) reply.ui.MergeFrom(say.ui());
    if (say.has_follow_up_listen_ms()) reply.followUpListenMs = say.follow_up_listen_ms();
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
    _questionChoices.clear();
    _expectation = formExpectation();
    if (const auto* slot = askedSlot()) reply.choices = slotChoices(*slot);
    reply.expectsReply = true;
    return question;
}

// What the form is asking for: what to change, a slot, or a yes to the read-back.
Expectation FlowRuntime::formExpectation() const {
    Expectation expectation;
    if (_form->awaitingChange) {
        expectation.set_mode(Expectation::OPEN);
    } else if (const auto* slot = askedSlot()) {
        expectation = slotExpectation(*slot);
    } else if (_form->awaitingConfirmation) {
        expectation.set_mode(Expectation::CONFIRMATION);
    }
    return expectation;
}

const vasdk::goal::Slot* FlowRuntime::askedSlot() const {
    if (!_form || _form->asking.empty() || _form->awaitingChange || _form->awaitingConfirmation) return nullptr;
    for (const auto& slot : _form->goal->slots())
        if (slot.name() == _form->asking) return &slot;
    return nullptr;
}

// A slot's values as buttons: its list entity's or ENUM values, when there are few.
std::vector<ReplyChoice> FlowRuntime::slotChoices(const vasdk::goal::Slot& slot) const {
    std::vector<ReplyChoice> choices;
    if (!slot.entity().empty()) {
        for (const auto& value : _agent->entities().values(entityName(slot.entity()), _dynamicEntities))
            choices.push_back({value.spoken().empty() ? value.value() : value.spoken(), value.value()});
    } else if (slot.type() == vasdk::goal::Slot::ENUM) {
        for (const auto& value : slot.values()) choices.push_back({value, value});
    }
    if (choices.size() > kMaxSlotChoices) choices.clear();
    return choices;
}

// The most digits said or written in a row ("five five five, one two" → 5).
size_t FlowRuntime::digitCount(const std::string& text) const {
    const auto run = _agent->entities().find("sys.digits", text);
    return run ? run->value.string_value().size() : 0;
}

// The answer to the slot asked: its digits when DIGITS are expected of a plain slot, else as
// readSlot reads it.
std::optional<std::string> FlowRuntime::readAnswer(const vasdk::goal::Slot& slot, const std::string& text) const {
    using Slot = vasdk::goal::Slot;
    if (_expectation.mode() != Expectation::DIGITS || !slot.entity().empty() ||
        (slot.type() != Slot::STRING && slot.type() != Slot::ENUM))
        return readSlot(slot, text);
    const auto run = _agent->entities().find("sys.digits", text);
    if (!run) return std::nullopt;
    const auto& digits = run->value.string_value();
    const auto count = static_cast<int>(digits.size());
    if ((_expectation.has_min_digits() && count < _expectation.min_digits()) ||
        (_expectation.has_max_digits() && count > _expectation.max_digits()))
        return std::nullopt;
    return digits;
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
                if (const auto value = readAnswer(*slot, text))
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
    const int limit = form.goal->max_reprompts() > 0 && !_expectation.has_max_reprompts() ? form.goal->max_reprompts()
                                                                                          : maxReprompts();
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
        // Too few digits: say so, rather than that the answer wasn't understood.
        const auto digits = digitCount(_inputText);
        if (_expectation.mode() == Expectation::DIGITS && digits > 0 && _expectation.has_min_digits() &&
            digits < static_cast<size_t>(_expectation.min_digits())) {
            const auto& partial = slot->expect().partial_reprompt();
            if (!partial.empty()) {
                ask(partial, &partial, "", reply);
                return {};
            }
            append(reply, "I only got " + std::to_string(digits) + (digits == 1 ? " digit." : " digits."));
            ask(slot->prompt(), &slot->prompt(), "What's the " + spokenName(slot->name()) + "?", reply);
            return {};
        }
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
        _questionChoices.clear();
        _expectation.Clear();
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
        // The page type's steps first, then the page's own.
        const auto& extensions = _agent->extensions(_position);
        Outcome outcome;
        if (extensions.declaredType) {
            outcome = run(extensions.declaredType->on_enter(), reply);
        } else if (extensions.registeredType && extensions.registeredType->onEnter) {
            TurnContext turn(*this, reply);
            extensions.registeredType->onEnter(paramsOf(extensions), turn);
            outcome = fromContext(turn);
        }
        if (!outcome.moves()) outcome = run(_agent->page(_position).on_enter(), reply);
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
