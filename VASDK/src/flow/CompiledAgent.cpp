#include "flow/CompiledAgent.h"

#include "engine/ProtoFileLoader.h"
#include "flow/SlotExtraction.h"
#include "goal/GoalState.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>

namespace va {
namespace flow {

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        // Letters, digits, apostrophes ("don't") and any non-ASCII byte (accented letters) make words.
        if (std::isalnum(byte) || c == '\'' || byte >= 0x80) {
            word += static_cast<char>(std::tolower(byte));
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

namespace {

std::string quoted(const std::string& name) { return "\"" + name + "\""; }

goal::SlotType slotType(vasdk::goal::Slot::Type type) {
    using Type = vasdk::goal::Slot;
    switch (type) {
    case Type::PHONE: return goal::SlotType::PHONE;
    case Type::EMAIL: return goal::SlotType::EMAIL;
    case Type::DATE: return goal::SlotType::DATE;
    case Type::TIME: return goal::SlotType::TIME;
    case Type::DATETIME: return goal::SlotType::DATETIME;
    case Type::NUMBER: return goal::SlotType::NUMBER;
    case Type::INTEGER: return goal::SlotType::INTEGER;
    case Type::BOOLEAN: return goal::SlotType::BOOLEAN;
    case Type::ENUM: return goal::SlotType::ENUM;
    default: return goal::SlotType::STRING;
    }
}

// The GoalState type that stores an entity's values.
goal::SlotType entitySlotType(const EntityCatalog& entities, const std::string& name) {
    std::string base = name;
    if (const auto* type = entities.type(name); type && type->has_derived()) base = entityName(type->derived().base());
    if (base == "sys.integer" || base == "sys.number") return goal::SlotType::NUMBER;
    if (base == "sys.phone") return goal::SlotType::PHONE;
    if (base == "sys.yes_no") return goal::SlotType::BOOLEAN;
    return goal::SlotType::STRING;
}

bool isFormStatus(const std::string& field) { return field == "complete" || field == "confirmed" || field == "failed"; }

bool hasSlot(const vasdk::goal::GoalDefinition& goal, const std::string& name) {
    return std::any_of(goal.slots().begin(), goal.slots().end(), [&](const auto& slot) { return slot.name() == name; });
}

class Checker {
public:
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    void error(const std::string& where, const std::string& message) { errors.push_back(where + ": " + message); }
    void warn(const std::string& where, const std::string& message) { warnings.push_back(where + ": " + message); }
};

} // namespace

std::shared_ptr<const CompiledAgent> CompiledAgent::load(const std::string& path, std::string* error,
                                                         std::vector<std::string>* warnings) {
    vasdk::flow::Agent agent;
    // No ${ENV} expansion: secrets belong in the engine config, never in an agent.
    if (!engine::loadProtoFile(path, agent, error)) return nullptr;
    const auto directory = std::filesystem::path(path).parent_path();
    for (const auto& file : agent.goal_files()) {
        vasdk::goal::GoalDefinition goal;
        std::string goalError;
        if (!engine::loadProtoFile((directory / file).string(), goal, &goalError)) {
            if (error) *error = "goal_files " + quoted(file) + ": " + goalError;
            return nullptr;
        }
        *agent.add_goals() = std::move(goal);
    }
    return compile(std::move(agent), error, warnings);
}

std::shared_ptr<const CompiledAgent> CompiledAgent::compile(vasdk::flow::Agent agent, std::string* error,
                                                            std::vector<std::string>* warnings) {
    std::shared_ptr<CompiledAgent> compiled(new CompiledAgent());
    compiled->_agent = std::move(agent);
    const auto& definition = compiled->_agent;
    Checker check;

    // Names first: targets and intents are resolved against them.
    for (int i = 0; i < definition.intents_size(); ++i) {
        const auto& intent = definition.intents(i);
        const std::string where = "intent " + (intent.name().empty() ? std::to_string(i + 1) : quoted(intent.name()));
        if (intent.name().empty()) check.error(where, "has no name");
        else if (compiled->_intentExamples.count(intent.name())) check.error(where, "is declared twice");
        auto& examples = compiled->_intentExamples[intent.name()];
        for (const auto& example : intent.examples()) {
            auto words = splitWords(example);
            if (words.empty()) check.error(where, "example " + quoted(example) + " has no words");
            else examples.push_back(std::move(words));
        }
        if (intent.examples().empty()) check.error(where, "has no examples");
    }
    for (int i = 0; i < definition.session_params_size(); ++i) {
        const auto& param = definition.session_params(i);
        const std::string where = "session param " + (param.name().empty() ? std::to_string(i + 1) : quoted(param.name()));
        if (param.name().empty()) check.error(where, "has no name");
        else if (!compiled->_sessionParams.emplace(param.name(), i).second) check.error(where, "is declared twice");
        if (!fitsType(param.type(), param.default_()))
            check.error(where, "default doesn't fit its type " + vasdk::flow::SessionParam::Type_Name(param.type()));
    }
    for (int i = 0; i < definition.goals_size(); ++i) {
        const auto& goal = definition.goals(i);
        const std::string where = "goal " + (goal.name().empty() ? std::to_string(i + 1) : quoted(goal.name()));
        if (goal.name().empty()) check.error(where, "has no name");
        else if (!compiled->_goals.emplace(goal.name(), i).second) check.error(where, "is declared twice");
    }

    {
        std::vector<std::string> entityErrors;
        compiled->_entities.build(definition.entity_types(), entityErrors);
        check.errors.insert(check.errors.end(), entityErrors.begin(), entityErrors.end());
    }
    auto checkEntity = [&](const std::string& reference, const std::string& where) {
        if (!compiled->_entities.knows(entityName(reference)))
            check.error(where, "unknown entity " + quoted(reference) + " (declare it in entity_types, or use @sys.integer, "
                                                                        "@sys.number, @sys.phone or @sys.yes_no)");
    };

    // The goal whose slots $form.<slot> names: the page's form, or the goal being checked.
    const vasdk::goal::GoalDefinition* formGoal = nullptr;
    // The entity $entity reads: a route's, while its condition and steps are checked.
    std::string routeEntity;

    // References must name something that exists: a declared session param, a page field, ...
    auto checkReferences = [&](const std::vector<Path>& references, const std::string& where) {
        for (const auto& path : references) {
            const std::string& root = path[0];
            const std::string field = path.size() > 1 ? path[1] : "";
            bool known = true;
            if (root == "session") {
                if (!compiled->_sessionParams.count(field)) {
                    check.error(where, pathText(path) + ": no session param " + quoted(field) + " is declared");
                    continue;
                }
            } else if (root == "page") {
                known = (field == "name" && path.size() == 2) || (field == "locals" && path.size() >= 3);
            } else if (root == "input") {
                known = field == "text" && path.size() == 2;
            } else if (root == "nlu") {
                known = (field == "intent" && path.size() == 2) ||
                        (field == "entities" && path.size() >= 3 && compiled->_entities.knows(path[2]));
            } else if (root == "entity") {
                if (routeEntity.empty()) {
                    check.error(where, pathText(path) + ": $entity is read only in a route with an entity");
                    continue;
                }
            } else if (root == "entities") {
                const auto* type = compiled->_entities.type(field);
                known = path.size() == 2 && type && type->has_list();
            } else if (root == "context") {
                known = path.size() >= 2;
            } else if (root == "form") {
                if (!formGoal) {
                    check.error(where, pathText(path) + ": the page has no form");
                    continue;
                }
                const auto slot = std::find_if(formGoal->slots().begin(), formGoal->slots().end(),
                                               [&](const auto& candidate) { return candidate.name() == field; });
                known = (path.size() == 2 && (slot != formGoal->slots().end() || isFormStatus(field))) ||
                        (path.size() > 2 && slot != formGoal->slots().end() && !slot->entity().empty());
            } else {
                known = false;
            }
            if (!known)
                check.error(where, "unknown reference " + pathText(path) +
                                       " (use $session.<name>, $page.name, $page.locals.<name>, $input.text, "
                                       "$nlu.intent, $nlu.entities.<entity>, $entities.<list entity>, $context.<name> "
                                       "or, on a form page, $form.<slot>)");
        }
    };
    auto compileExpression = [&](const std::string& text, const std::string& where) {
        if (text.empty()) {
            check.error(where, "is empty");
            return std::shared_ptr<const Expression>();
        }
        std::string parseError;
        auto expression = Expression::parse(text, &parseError);
        if (!expression) {
            check.error(where, quoted(text) + ": " + parseError);
            return expression;
        }
        checkReferences(expression->references(), where);
        compiled->_expressions.emplace(text, expression);
        return expression;
    };
    auto checkTemplate = [&](const std::string& text, const std::string& where) {
        std::vector<Path> references;
        std::string templateError;
        if (!templateReferences(text, references, &templateError)) check.error(where, templateError);
        checkReferences(references, where);
    };

    for (const auto& goal : definition.goals()) {
        const std::string where = "goal " + quoted(goal.name());
        formGoal = &goal;
        auto built = std::make_shared<goal::GoalDefinition>();
        built->name = goal.name();
        built->objective = goal.objective();
        built->requireConfirmation = goal.require_confirmation();
        std::set<std::string> slotNames;
        for (int i = 0; i < goal.slots_size(); ++i) {
            const auto& slot = goal.slots(i);
            const std::string slotWhere = where + " slot " + (slot.name().empty() ? std::to_string(i + 1) : quoted(slot.name()));
            if (slot.name().empty()) check.error(slotWhere, "has no name");
            else if (!slotNames.insert(slot.name()).second) check.error(slotWhere, "is declared twice");
            else if (isFormStatus(slot.name())) check.error(slotWhere, "is a reserved name ($form.complete, confirmed and failed)");
            const bool isEnum = slot.type() == vasdk::goal::Slot::ENUM;
            if (!slot.entity().empty()) {
                checkEntity(slot.entity(), slotWhere);
                if (!slot.values().empty()) check.error(slotWhere, "has both an entity and values");
            } else if (isEnum && slot.values().empty()) {
                check.error(slotWhere, "is an ENUM with no values (or give it an entity)");
            } else if (!isEnum && !slot.values().empty()) {
                check.warn(slotWhere, "has values, but only ENUM slots use them");
            }
            if (slot.has_min() && slot.has_max() && slot.min() > slot.max()) check.error(slotWhere, "min is above max");
            for (const auto& text : slot.prompt()) checkTemplate(text, slotWhere + " prompt");
            for (const auto& text : slot.reprompt()) checkTemplate(text, slotWhere + " reprompt");

            goal::GoalSlot running;
            running.name = slot.name();
            running.description = slot.description();
            running.type = slot.entity().empty() ? slotType(slot.type())
                                                 : entitySlotType(compiled->_entities, entityName(slot.entity()));
            running.required = !slot.optional();
            running.constraints.allowedValues.assign(slot.values().begin(), slot.values().end());
            if (slot.has_min()) running.constraints.min = slot.min();
            if (slot.has_max()) running.constraints.max = slot.max();
            running.constraints.mustBeFuture = slot.must_be_future();
            if (!slot.condition().empty()) {
                if (auto expression = compileExpression(slot.condition(), slotWhere + " condition")) {
                    for (const auto& path : expression->references())
                        if (path[0] != "form" || isFormStatus(path.size() > 1 ? path[1] : ""))
                            check.error(slotWhere + " condition", pathText(path) + ": a slot condition reads only $form.<slot>");
                    running.condition = [expression](const goal::GoalState& state) {
                        return isTruthy(expression->evaluate([&state](const Path& path) {
                            return path.size() == 2 && path[0] == "form" ? slotValue(state, path[1]) : nullValue();
                        }));
                    };
                }
            }
            built->slots.push_back(std::move(running));
        }
        if (goal.slots().empty()) check.error(where, "has no slots");
        if (goal.require_confirmation() && goal.confirm().empty())
            check.error(where, "has require_confirmation but no confirm text to read back");
        for (const auto& text : goal.confirm()) checkTemplate(text, where + " confirm");
        for (const auto& text : goal.change()) checkTemplate(text, where + " change");
        if (goal.max_reprompts() < 0) check.error(where, "max_reprompts is negative");
        if (!goal.name().empty()) compiled->_goalDefinitions[goal.name()] = std::move(built);
    }
    formGoal = nullptr;

    compiled->_pages.resize(definition.flows_size());
    for (int f = 0; f < definition.flows_size(); ++f) {
        const auto& flow = definition.flows(f);
        const std::string where = "flow " + (flow.name().empty() ? std::to_string(f + 1) : quoted(flow.name()));
        if (flow.name().empty()) check.error(where, "has no name");
        else if (!compiled->_flows.emplace(flow.name(), f).second) check.error(where, "is declared twice");
        for (int p = 0; p < flow.pages_size(); ++p) {
            const auto& name = flow.pages(p).name();
            if (name.empty()) check.error(where + " page " + std::to_string(p + 1), "has no name");
            else if (!compiled->_pages[f].emplace(name, p).second)
                check.error(where + " page " + quoted(name), "is declared twice");
        }
        if (flow.start_page().empty()) check.error(where, "has no start_page");
        else if (!compiled->_pages[f].count(flow.start_page()))
            check.error(where, "start_page " + quoted(flow.start_page()) + " is not one of its pages");
    }
    if (definition.start_flow().empty()) {
        check.error("agent", "has no start_flow");
    } else if (auto start = compiled->resolve("flow:" + definition.start_flow(), {}); start) {
        compiled->_startPage = *start;
    } else if (!compiled->_flows.count(definition.start_flow())) {
        check.error("agent", "start_flow " + quoted(definition.start_flow()) + " is not one of its flows");
    }

    // Envelopes by name, and their extends chains.
    for (int e = 0; e < definition.envelope_defs_size(); ++e) {
        const auto& envelope = definition.envelope_defs(e);
        const std::string where = "envelope " + (envelope.name().empty() ? std::to_string(e + 1) : quoted(envelope.name()));
        if (envelope.name().empty()) check.error(where, "has no name");
        else if (!compiled->_envelopes.emplace(envelope.name(), e).second) check.error(where, "is declared twice");
    }
    for (const auto& envelope : definition.envelope_defs()) {
        std::set<std::string> chain{envelope.name()};
        for (const auto* at = &envelope; !at->extends().empty();) {
            const auto base = compiled->_envelopes.find(at->extends());
            if (base == compiled->_envelopes.end()) {
                check.error("envelope " + quoted(at->name()), "extends unknown envelope " + quoted(at->extends()));
                break;
            }
            if (!chain.insert(at->extends()).second) {
                check.error("envelope " + quoted(envelope.name()), "extends itself through " + quoted(at->extends()));
                break;
            }
            at = &definition.envelope_defs(base->second);
        }
    }

    // Steps, routes and handlers, collecting where each page leads for the reachability check.
    std::set<std::string> usedIntents, usedGoals, usedEnvelopes;
    std::vector<std::vector<std::vector<PageRef>>> leadsTo(definition.flows_size());
    std::vector<PageRef> reachedFromAnywhere;   // flows an envelope can enter
    PageRef here;              // the page being checked
    bool inEnvelope = false;   // envelope targets can't name a page: it would differ on each page
    auto checkTarget = [&](const std::string& target, const std::string& at) {
        if (target.empty() || target == kTargetEndSession || target == kTargetEndFlow) return;
        if (inEnvelope) {
            if (target == kTargetCurrentPage) return;
            if (auto next = target.rfind("flow:", 0) == 0 ? compiled->resolve(target, {}) : std::nullopt) {
                reachedFromAnywhere.push_back(*next);
                return;
            }
            check.error(at, "unknown target " + quoted(target) +
                                " (an envelope's targets are flow:<name>, END_FLOW, END_SESSION or CURRENT_PAGE)");
            return;
        }
        if (auto next = compiled->resolve(target, here)) leadsTo[here.flow][here.page].push_back(*next);
        else check.error(at, "unknown target " + quoted(target));
    };
    auto checkWritable = [&](const std::string& key, const std::string& at, const std::string& label) {
        auto expression = Expression::parse(key);
        const Path* path = expression ? expression->reference() : nullptr;
        const bool writable = path && ((path->size() == 2 && (*path)[0] == "session") ||
                                       (path->size() == 3 && (*path)[0] == "page" && (*path)[1] == "locals") ||
                                       (path->size() == 2 && (*path)[0] == "form" && !isFormStatus((*path)[1])));
        if (!writable)
            check.error(at, label + " " + quoted(key) + " must be $session.<name>, $page.locals.<name> or $form.<slot>");
        else
            checkReferences({*path}, at + " " + label);
    };
    auto checkSay = [&](const vasdk::flow::Say& say, const std::string& at, const std::string& kind) {
        if (say.text().empty() && say.ssml().empty()) check.error(at, kind + " has no text");
        for (const auto& text : say.text()) checkTemplate(text, at);
        if (!say.ssml().empty()) checkTemplate(say.ssml(), at + " ssml");
    };
    using Steps = google::protobuf::RepeatedPtrField<vasdk::flow::Step>;
    std::function<void(const Steps&, const std::string&)> checkSteps = [&](const Steps& steps, const std::string& at) {
        for (int s = 0; s < steps.size(); ++s) {
            const auto& step = steps.Get(s);
            const std::string stepWhere = at + " step " + std::to_string(s + 1);
            switch (step.kind_case()) {
            case vasdk::flow::Step::kSay:
                checkSay(step.say(), stepWhere, "say");
                break;
            case vasdk::flow::Step::kAsk:
                checkSay(step.ask().say(), stepWhere, "ask");
                for (int c = 0; c < step.ask().choices_size(); ++c)
                    if (step.ask().choices(c).label().empty())
                        check.error(stepWhere, "choice " + std::to_string(c + 1) + " has no label");
                break;
            case vasdk::flow::Step::kSet:
                compileExpression(step.set().value(), stepWhere + " set value");
                checkWritable(step.set().key(), stepWhere, "set key");
                break;
            case vasdk::flow::Step::kExtract:
                checkEntity(step.extract().entity(), stepWhere + " extract");
                checkWritable(step.extract().into(), stepWhere, "extract into");
                break;
            case vasdk::flow::Step::kBranch:
                compileExpression(step.branch().if_(), stepWhere + " branch if");
                checkSteps(step.branch().then(), stepWhere + " then");
                checkSteps(step.branch().else_(), stepWhere + " else");
                break;
            case vasdk::flow::Step::kSwitch:
                compileExpression(step.switch_().on(), stepWhere + " switch on");
                for (int c = 0; c < step.switch_().cases_size(); ++c)
                    checkSteps(step.switch_().cases(c).steps(), stepWhere + " case " + std::to_string(c + 1));
                checkSteps(step.switch_().default_(), stepWhere + " default");
                break;
            case vasdk::flow::Step::kClearSlot:
                if (!formGoal) check.error(stepWhere, "clear_slot needs a form on the page");
                else if (!hasSlot(*formGoal, step.clear_slot().slot()))
                    check.error(stepWhere, "clear_slot: goal " + quoted(formGoal->name()) + " has no slot " +
                                               quoted(step.clear_slot().slot()));
                break;
            case vasdk::flow::Step::kGoto:
                if (step.goto_().target().empty()) check.error(stepWhere, "goto has no target");
                checkTarget(step.goto_().target(), stepWhere);
                break;
            case vasdk::flow::Step::kEnd:
                break;
            case vasdk::flow::Step::KIND_NOT_SET:
                check.error(stepWhere, "is empty");
                break;
            }
        }
    };
    auto checkRoutes = [&](const google::protobuf::RepeatedPtrField<vasdk::flow::Route>& routes, const std::string& where) {
        for (int r = 0; r < routes.size(); ++r) {
            const auto& route = routes.Get(r);
            const std::string routeWhere = where + " route " + std::to_string(r + 1);
            if (route.intent().empty() && route.entity().empty() && route.condition().empty())
                check.error(routeWhere, "has no intent, entity or condition");
            else if (!route.intent().empty() && !compiled->_intentExamples.count(route.intent()))
                check.error(routeWhere, "unknown intent " + quoted(route.intent()));
            if (!route.entity().empty()) checkEntity(route.entity(), routeWhere);
            routeEntity = route.entity();
            if (!route.condition().empty()) compileExpression(route.condition(), routeWhere + " condition");
            usedIntents.insert(route.intent());
            checkSteps(route.steps(), routeWhere);
            routeEntity.clear();
            checkTarget(route.target(), routeWhere);
        }
    };
    auto checkHandlers = [&](const google::protobuf::RepeatedPtrField<vasdk::flow::Handler>& handlers, const std::string& where) {
        for (int h = 0; h < handlers.size(); ++h) {
            const auto& handler = handlers.Get(h);
            const std::string handlerWhere = where + " handler " + std::to_string(h + 1);
            if (handler.event() != kEventNoMatch && handler.event() != kEventNoInput && handler.event() != kEventMaxReprompts)
                check.error(handlerWhere, "unknown event " + quoted(handler.event()) + " (no-match, no-input or max-reprompts)");
            checkSteps(handler.steps(), handlerWhere);
            checkTarget(handler.target(), handlerWhere);
        }
    };
    auto checkEnvelopeNames = [&](const google::protobuf::RepeatedPtrField<std::string>& names, const std::string& where) {
        for (const auto& name : names) {
            usedEnvelopes.insert(name);
            if (!compiled->_envelopes.count(name)) check.error(where, "unknown envelope " + quoted(name));
        }
    };

    // Envelopes are checked once, for no page in particular.
    inEnvelope = true;
    for (const auto& envelope : definition.envelope_defs()) {
        const std::string where = "envelope " + quoted(envelope.name());
        checkRoutes(envelope.routes(), where);
        checkHandlers(envelope.handlers(), where);
        if (envelope.settings().has_no_input_ms() && envelope.settings().no_input_ms() < 0)
            check.error(where, "no_input_ms is negative");
        if (envelope.settings().has_max_reprompts() && envelope.settings().max_reprompts() < 0)
            check.error(where, "max_reprompts is negative");
    }
    inEnvelope = false;
    checkEnvelopeNames(definition.envelopes(), "agent envelopes");

    for (int f = 0; f < definition.flows_size(); ++f) {
        const auto& flow = definition.flows(f);
        leadsTo[f].resize(flow.pages_size());
        checkEnvelopeNames(flow.envelopes(), "flow " + quoted(flow.name()) + " envelopes");
        for (int p = 0; p < flow.pages_size(); ++p) {
            const auto& page = flow.pages(p);
            here = PageRef{f, p};
            const std::string where = "flow " + quoted(flow.name()) + " page " + quoted(page.name());
            formGoal = nullptr;
            if (page.has_form()) {
                formGoal = compiled->goal(page.form().goal());
                usedGoals.insert(page.form().goal());
                if (page.form().goal().empty()) check.error(where, "form has no goal");
                else if (!formGoal) check.error(where, "form: unknown goal " + quoted(page.form().goal()));
            }
            checkEnvelopeNames(page.envelopes(), where + " envelopes");
            checkSteps(page.on_enter(), where + " on_enter");
            checkRoutes(page.routes(), where);
            checkHandlers(page.handlers(), where);
            const bool leavesOnEntry = std::any_of(page.on_enter().begin(), page.on_enter().end(), [](const auto& step) {
                return step.has_goto_() || step.has_end();
            });
            const bool sharesRoutes = !page.envelopes().empty() || !flow.envelopes().empty() || !definition.envelopes().empty();
            if (page.routes().empty() && !leavesOnEntry && !page.has_form() && !sharesRoutes)
                check.warn(where, "has no routes: every answer on it is not understood");
            const bool hasConditionRoute = std::any_of(page.routes().begin(), page.routes().end(), [](const auto& route) {
                return route.intent().empty() && route.entity().empty();
            });
            if (page.has_form() && !hasConditionRoute)
                check.warn(where, "has a form but no condition route: nothing happens once it is complete");
        }
    }
    formGoal = nullptr;
    for (const auto& envelope : definition.envelope_defs())
        if (!envelope.name().empty() && !usedEnvelopes.count(envelope.name())) {
            bool extended = false;
            for (const auto& other : definition.envelope_defs()) extended = extended || other.extends() == envelope.name();
            if (!extended) check.warn("envelope " + quoted(envelope.name()), "is not applied anywhere");
        }

    // Each page's layers, innermost first: its own, then its envelopes, its flow's and the agent's,
    // each followed by what it extends.
    if (check.errors.empty()) {
        compiled->_layers.resize(definition.flows_size());
        for (int f = 0; f < definition.flows_size(); ++f) {
            const auto& flow = definition.flows(f);
            for (const auto& page : flow.pages()) {
                std::vector<Layer> layers{{&page.routes(), &page.handlers(), nullptr}};
                std::set<std::string> seen;
                auto add = [&](const google::protobuf::RepeatedPtrField<std::string>& names) {
                    for (const auto& name : names)
                        for (std::string at = name; !at.empty() && seen.insert(at).second;) {
                            const auto& envelope = definition.envelope_defs(compiled->_envelopes.at(at));
                            layers.push_back({&envelope.routes(), &envelope.handlers(), &envelope.settings()});
                            at = envelope.extends();
                        }
                };
                add(page.envelopes());
                add(flow.envelopes());
                add(definition.envelopes());
                compiled->_layers[f].push_back(std::move(layers));
            }
        }
    }

    if (compiled->_startPage.valid()) {
        std::set<std::pair<int, int>> reached{{compiled->_startPage.flow, compiled->_startPage.page}};
        std::deque<PageRef> pending{compiled->_startPage};
        for (const auto& entered : reachedFromAnywhere)
            if (reached.insert({entered.flow, entered.page}).second) pending.push_back(entered);
        while (!pending.empty()) {
            const PageRef at = pending.front();
            pending.pop_front();
            for (const auto& next : leadsTo[at.flow][at.page])
                if (reached.insert({next.flow, next.page}).second) pending.push_back(next);
        }
        for (int f = 0; f < definition.flows_size(); ++f)
            for (int p = 0; p < definition.flows(f).pages_size(); ++p)
                if (!reached.count({f, p}))
                    check.warn("flow " + quoted(definition.flows(f).name()) + " page " +
                                   quoted(definition.flows(f).pages(p).name()),
                               "is never reached from the start page");
    }
    for (const auto& intent : definition.intents())
        if (!intent.name().empty() && !usedIntents.count(intent.name()))
            check.warn("intent " + quoted(intent.name()), "is not used by any route");
    for (const auto& goal : definition.goals())
        if (!goal.name().empty() && !usedGoals.count(goal.name()))
            check.warn("goal " + quoted(goal.name()), "is not used by any form");

    if (warnings) warnings->insert(warnings->end(), check.warnings.begin(), check.warnings.end());
    if (!check.errors.empty()) {
        if (error) {
            error->clear();
            for (const auto& line : check.errors) *error += (error->empty() ? "" : "\n") + line;
        }
        return nullptr;
    }
    return compiled;
}

std::optional<CompiledAgent::PageRef> CompiledAgent::resolve(const std::string& target, PageRef from) const {
    if (target == kTargetCurrentPage) return from.valid() ? std::optional<PageRef>(from) : std::nullopt;
    static const std::string flowPrefix = "flow:";
    if (target.compare(0, flowPrefix.size(), flowPrefix) == 0) {
        const auto flow = _flows.find(target.substr(flowPrefix.size()));
        if (flow == _flows.end()) return std::nullopt;
        const auto page = _pages[flow->second].find(_agent.flows(flow->second).start_page());
        if (page == _pages[flow->second].end()) return std::nullopt;
        return PageRef{flow->second, page->second};
    }
    if (from.flow < 0) return std::nullopt;
    const auto page = _pages[from.flow].find(target);
    if (page == _pages[from.flow].end()) return std::nullopt;
    return PageRef{from.flow, page->second};
}

const vasdk::flow::SessionParam* CompiledAgent::sessionParam(const std::string& name) const {
    const auto found = _sessionParams.find(name);
    return found == _sessionParams.end() ? nullptr : &_agent.session_params(found->second);
}

std::shared_ptr<const Expression> CompiledAgent::expression(const std::string& text) const {
    const auto found = _expressions.find(text);
    return found == _expressions.end() ? nullptr : found->second;
}

std::string entityName(const std::string& reference) {
    return !reference.empty() && reference[0] == '@' ? reference.substr(1) : std::string();
}

const vasdk::goal::GoalDefinition* CompiledAgent::goal(const std::string& name) const {
    const auto found = _goals.find(name);
    return found == _goals.end() ? nullptr : &_agent.goals(found->second);
}

std::unique_ptr<goal::GoalState> CompiledAgent::newGoalState(const std::string& name) const {
    const auto found = _goalDefinitions.find(name);
    return found == _goalDefinitions.end() ? nullptr : std::make_unique<goal::GoalState>(*found->second);
}

bool fitsType(vasdk::flow::SessionParam::Type type, const Value& value) {
    switch (value.kind_case()) {
    case Value::KIND_NOT_SET:
    case Value::kNullValue: return true;
    case Value::kStringValue: return type == vasdk::flow::SessionParam::STRING;
    case Value::kNumberValue: return type == vasdk::flow::SessionParam::NUMBER;
    case Value::kBoolValue: return type == vasdk::flow::SessionParam::BOOL;
    case Value::kListValue: return type == vasdk::flow::SessionParam::LIST;
    default: return false;
    }
}

int CompiledAgent::matchIntent(const std::string& intent, const std::vector<std::string>& words) const {
    const auto found = _intentExamples.find(intent);
    if (found == _intentExamples.end()) return 0;
    int best = 0;
    for (const auto& example : found->second)
        if (static_cast<int>(example.size()) > best &&
            std::search(words.begin(), words.end(), example.begin(), example.end()) != words.end())
            best = static_cast<int>(example.size());
    return best;
}

} // namespace flow
} // namespace va
