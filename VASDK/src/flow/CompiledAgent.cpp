#include "flow/CompiledAgent.h"

#include "engine/ProtoFileLoader.h"
#include "flow/SlotExtraction.h"
#include "goal/GoalState.h"

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

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

// Key-like literals: API keys and tokens of common services, private keys, bearer tokens.
const std::regex& secretPattern() {
    static const std::regex pattern(
        R"(sk-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9]{30,}|xox[abprs]-[A-Za-z0-9-]{10,})"
        R"(|AIza[0-9A-Za-z_-]{35}|-----BEGIN [A-Z ]*PRIVATE KEY-----|[Bb]earer +[A-Za-z0-9._~+/=-]{16,})");
    return pattern;
}

// Calls `visit` with each string field in `message`, and where it is ("tools[1].http.headers[2].value").
void forEachString(const google::protobuf::Message& message, const std::string& at,
                   const std::function<void(const std::string& text, const std::string& where)>& visit) {
    const auto* reflection = message.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    reflection->ListFields(message, &fields);
    for (const auto* field : fields) {
        const std::string name = (at.empty() ? "" : at + ".") + std::string(field->name());
        const bool repeated = field->is_repeated();
        const int count = repeated ? reflection->FieldSize(message, field) : 1;
        for (int i = 0; i < count; ++i) {
            const std::string where = repeated ? name + "[" + std::to_string(i + 1) + "]" : name;
            if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
                field->type() != google::protobuf::FieldDescriptor::TYPE_BYTES) {
                visit(repeated ? reflection->GetRepeatedString(message, field, i) : reflection->GetString(message, field), where);
            } else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                forEachString(repeated ? reflection->GetRepeatedMessage(message, field, i) : reflection->GetMessage(message, field),
                              where, visit);
            }
        }
    }
}

class Checker {
public:
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    void error(const std::string& where, const std::string& message) { errors.push_back(where + ": " + message); }
    void warn(const std::string& where, const std::string& message) { warnings.push_back(where + ": " + message); }
};

} // namespace

// The types of proto_imports: the .proto files next to the agent, which may import those linked
// into the binary (google/protobuf/*.proto, vasdk/...).
struct CompiledAgent::ImportedTypes {
    class Errors : public google::protobuf::compiler::MultiFileErrorCollector {
    public:
        void RecordError(absl::string_view file, int line, int column, absl::string_view message) override {
            text += (text.empty() ? "" : "\n") + std::string(file) + ":" + std::to_string(line + 1) + ":" +
                    std::to_string(column + 1) + ": " + std::string(message);
        }
        std::string text;
    };
    explicit ImportedTypes(const std::string& directory) : files(&sourceTree, &linked), merged(&linked, &files), pool(&merged) {
        sourceTree.MapPath("", directory.empty() ? "." : directory);
        files.RecordErrorsTo(&errors);
    }
    google::protobuf::compiler::DiskSourceTree sourceTree;
    google::protobuf::DescriptorPoolDatabase linked{*google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::compiler::SourceTreeDescriptorDatabase files;
    google::protobuf::MergedDescriptorDatabase merged;
    google::protobuf::DescriptorPool pool;
    mutable google::protobuf::DynamicMessageFactory factory{&pool};   // thread-safe; GetPrototype isn't const
    Errors errors;
};

namespace {

// An Any's type, by its full name: "type.googleapis.com/acme.Points" → "acme.Points".
std::string typeName(const google::protobuf::Any& any) {
    const auto slash = any.type_url().rfind('/');
    return slash == std::string::npos ? any.type_url() : any.type_url().substr(slash + 1);
}

// An agent file's proto_imports, read before the file itself can be: its params' types may be in them.
std::vector<std::string> readProtoImports(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    std::vector<std::string> imports;
    const auto extension = std::filesystem::path(path).extension().string();
    vasdk::flow::Agent agent;
    if (extension == ".pb" || extension == ".binpb") {
        agent.ParseFromString(content);
    } else if (extension == ".json") {
        const auto json = nlohmann::json::parse(content, nullptr, false);
        for (const char* key : {"protoImports", "proto_imports"})
            if (json.is_object() && json.contains(key) && json[key].is_array())
                for (const auto& name : json[key])
                    if (name.is_string()) imports.push_back(name.get<std::string>());
        return imports;
    } else if (content.find("proto_imports") != std::string::npos) {
        // Params of unknown types are read as empty messages, their fields skipped.
        class AnyAsEmpty : public google::protobuf::TextFormat::Finder {
            const google::protobuf::Descriptor* FindAnyType(const google::protobuf::Message& message, const std::string& prefix,
                                                            const std::string& name) const override {
                const auto* type = Finder::FindAnyType(message, prefix, name);
                return type ? type : vasdk::flow::End::descriptor();
            }
        } finder;
        google::protobuf::TextFormat::Parser parser;
        parser.SetFinder(&finder);
        parser.AllowUnknownField(true);
        class Quiet : public google::protobuf::io::ErrorCollector {
            void RecordError(int, google::protobuf::io::ColumnNumber, absl::string_view) override {}
        } quiet;
        parser.RecordErrorsTo(&quiet);
        parser.ParseFromString(content, &agent);   // errors are reported by the real parse
    }
    imports.assign(agent.proto_imports().begin(), agent.proto_imports().end());
    return imports;
}

} // namespace

std::shared_ptr<const CompiledAgent> CompiledAgent::load(const std::string& path, std::string* error,
                                                         std::vector<std::string>* warnings,
                                                         std::shared_ptr<const FlowRegistry> registry) {
    const auto directory = std::filesystem::path(path).parent_path();
    std::shared_ptr<ImportedTypes> imports;
    for (const auto& file : readProtoImports(path)) {
        if (!imports) imports = std::make_shared<ImportedTypes>(directory.string());
        if (!imports->pool.FindFileByName(file)) {
            if (error)
                *error = path + ": proto_imports " + quoted(file) + ": " +
                         (imports->errors.text.empty() ? "not found next to the agent" : imports->errors.text);
            return nullptr;
        }
    }
    vasdk::flow::Agent agent;
    // No ${ENV} expansion: secrets belong in the engine config, never in an agent.
    if (!engine::loadProtoFile(path, agent, error, false, imports ? &imports->pool : nullptr)) return nullptr;
    for (const auto& file : agent.goal_files()) {
        vasdk::goal::GoalDefinition goal;
        std::string goalError;
        if (!engine::loadProtoFile((directory / file).string(), goal, &goalError)) {
            if (error) *error = "goal_files " + quoted(file) + ": " + goalError;
            return nullptr;
        }
        *agent.add_goals() = std::move(goal);
    }
    return build(std::move(agent), std::move(imports), std::move(registry), error, warnings);
}

std::shared_ptr<const CompiledAgent> CompiledAgent::compile(vasdk::flow::Agent agent, std::string* error,
                                                            std::vector<std::string>* warnings,
                                                            std::shared_ptr<const FlowRegistry> registry) {
    return build(std::move(agent), nullptr, std::move(registry), error, warnings);
}

std::shared_ptr<const CompiledAgent> CompiledAgent::build(vasdk::flow::Agent agent, std::shared_ptr<const ImportedTypes> imports,
                                                          std::shared_ptr<const FlowRegistry> registry, std::string* error,
                                                          std::vector<std::string>* warnings) {
    std::shared_ptr<CompiledAgent> compiled(new CompiledAgent());
    compiled->_agent = std::move(agent);
    compiled->_imports = std::move(imports);
    compiled->_registry = registry ? std::move(registry) : FlowRegistry::defaults();
    const auto& definition = compiled->_agent;
    const auto& extensions = *compiled->_registry;
    Checker check;

    // A params message's type: linked into the binary, else from proto_imports.
    auto findType = [&](const std::string& name) -> const google::protobuf::Descriptor* {
        if (const auto* type = google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(name)) return type;
        return compiled->_imports ? compiled->_imports->pool.FindMessageTypeByName(name) : nullptr;
    };
    // Reads an Any's params once, for every conversation to use.
    auto readParams = [&](const google::protobuf::Any& any, const std::string& where) -> const google::protobuf::Message* {
        if (const auto found = compiled->_params.find(&any); found != compiled->_params.end()) return found->second.get();
        const std::string name = typeName(any);
        const auto* type = findType(name);
        if (!type) {
            check.error(where, "params type " + quoted(name) + " is unknown: list its .proto in proto_imports, or link it into the app");
            return nullptr;
        }
        const auto* prototype = type->file()->pool() == google::protobuf::DescriptorPool::generated_pool()
                                    ? google::protobuf::MessageFactory::generated_factory()->GetPrototype(type)
                                    : compiled->_imports->factory.GetPrototype(type);
        std::shared_ptr<google::protobuf::Message> params(prototype->New());
        if (!params->ParseFromString(any.value())) {
            check.error(where, "params aren't a valid " + quoted(name));
            return nullptr;
        }
        compiled->_params[&any] = params;
        return params.get();
    };

    // Names first: targets and intents are resolved against them.
    std::set<std::string> intentNames;
    for (int i = 0; i < definition.intents_size(); ++i) {
        const auto& intent = definition.intents(i);
        const std::string where = "intent " + (intent.name().empty() ? std::to_string(i + 1) : quoted(intent.name()));
        if (intent.name().empty()) check.error(where, "has no name");
        else if (!intentNames.insert(intent.name()).second) check.error(where, "is declared twice");
        for (const auto& example : intent.examples())
            if (splitWords(example).empty()) check.error(where, "example " + quoted(example) + " has no words");
        if (intent.examples().empty()) check.error(where, "has no examples");
    }
    compiled->_rules = std::make_shared<nlu::RuleNluProvider>(definition.intents());
    for (int i = 0; i < definition.session_params_size(); ++i) {
        const auto& param = definition.session_params(i);
        const std::string where = "session param " + (param.name().empty() ? std::to_string(i + 1) : quoted(param.name()));
        if (param.name().empty()) check.error(where, "has no name");
        else if (!compiled->_sessionParams.emplace(param.name(), i).second) check.error(where, "is declared twice");
        if (!fitsType(param.type(), param.default_()))
            check.error(where, "default doesn't fit its type " + vasdk::flow::SessionParam::Type_Name(param.type()));
    }
    // Secrets stay in the engine config: agent files are shared, committed and sent to tools.
    forEachString(definition, "", [&](const std::string& text, const std::string& where) {
        static const std::regex environment(R"(\$\{[A-Z_][A-Z0-9_]*(:-[^}]*)?\})");   // ${ACME_KEY}; ${session.x} is a reference
        if (std::regex_search(text, environment))
            check.error(where, "${...} reads the environment, which agent files can't: put secrets in the engine "
                               "config's security.secrets and read them in a tool header with $secrets.<name>");
        if (std::regex_search(text, secretPattern()))
            check.error(where, "looks like a secret (an API key or token): put it in the engine config's "
                               "security.secrets and read it in a tool header with $secrets.<name>");
        if (text.find("$secrets.") != std::string::npos && where.rfind("tools[", 0) != 0)
            check.error(where, "$secrets.<name> is only read in a tool's headers");
    });
    std::set<std::string> declaredTools;
    for (int i = 0; i < definition.tools_size(); ++i) {
        const auto& tool = definition.tools(i);
        const std::string where = "tool " + (tool.name().empty() ? std::to_string(i + 1) : quoted(tool.name()));
        if (tool.name().empty()) check.error(where, "has no name");
        else if (!declaredTools.insert(tool.name()).second) check.error(where, "is declared twice");
        const auto& url = tool.http().url();
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
            check.error(where, "http url must start with https:// (or http://)");
        if (!tool.http().method().empty() && tool.http().method() != "GET" && tool.http().method() != "POST")
            check.error(where, "http method must be GET or POST");
        for (const auto& [header, value] : tool.http().headers()) {
            std::string lower = header;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            const bool secretHeader = lower == "authorization" || lower.find("key") != std::string::npos ||
                                      lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos;
            if (secretHeader && value.find("$secrets.") == std::string::npos)
                check.error(where, "header " + quoted(header) + " holds a literal: read it from the engine config with $secrets.<name>");
        }
    }
    for (int i = 0; i < definition.goals_size(); ++i) {
        const auto& goal = definition.goals(i);
        const std::string where = "goal " + (goal.name().empty() ? std::to_string(i + 1) : quoted(goal.name()));
        if (goal.name().empty()) check.error(where, "has no name");
        else if (!compiled->_goals.emplace(goal.name(), i).second) check.error(where, "is declared twice");
    }

    std::map<std::string, const vasdk::flow::PageType*> pageTypes;
    for (int i = 0; i < definition.page_types_size(); ++i) {
        const auto& type = definition.page_types(i);
        const std::string where = "page type " + (type.name().empty() ? std::to_string(i + 1) : quoted(type.name()));
        if (type.name().empty()) check.error(where, "has no name");
        else if (!pageTypes.emplace(type.name(), &type).second) check.error(where, "is declared twice");
        if (!type.params_type().empty() && !findType(type.params_type()))
            check.error(where, "params_type " + quoted(type.params_type()) +
                                   " is unknown: list its .proto in proto_imports, or link it into the app");
    }

    {
        std::vector<std::string> entityErrors;
        compiled->_entities.build(definition.entity_types(), entityErrors);
        check.errors.insert(check.errors.end(), entityErrors.begin(), entityErrors.end());
    }
    auto checkEntity = [&](const std::string& reference, const std::string& where) {
        if (!compiled->_entities.knows(entityName(reference)))
            check.error(where, "unknown entity " + quoted(reference) + " (declare it in entity_types, or use @sys.integer, "
                                                                        "@sys.number, @sys.phone, @sys.digits or @sys.yes_no)");
    };
    for (const auto& entity : compiled->_rules->entitiesUsed()) checkEntity("@" + entity, "intent examples");

    // The goal whose slots $form.<slot> names: the page's form, or the goal being checked.
    const vasdk::goal::GoalDefinition* formGoal = nullptr;
    // The entity $entity reads: a route's, while its condition and steps are checked.
    std::string routeEntity;
    // What $params reads: the params of the page being checked.
    const google::protobuf::Descriptor* paramsType = nullptr;

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
                known = (field == "text" || field == "digit_count") && path.size() == 2;
            } else if (root == "nlu") {
                known = ((field == "intent" || field == "confidence" || field == "stage" || field == "alternatives") &&
                         path.size() == 2) ||
                        (field == "entities" && path.size() >= 3 && compiled->_entities.knows(path[2]));
            } else if (root == "entity") {
                if (routeEntity.empty()) {
                    check.error(where, pathText(path) + ": $entity is read only in a route with an entity");
                    continue;
                }
            } else if (root == "entities") {
                const auto* type = compiled->_entities.type(field);
                known = path.size() == 2 && type && type->has_list();
            } else if (root == "trigger") {
                known = (field == "name" && path.size() == 2) || (field == "payload" && path.size() >= 2);
            } else if (root == "context") {
                known = path.size() >= 2;
            } else if (root == "params") {
                if (!paramsType) {
                    check.error(where, pathText(path) + ": the page has no params");
                    continue;
                }
                known = paramsType->FindFieldByName(field) != nullptr;
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
                                       "$nlu.intent, $nlu.entities.<entity>, $entities.<list entity>, $context.<name>, "
                                       "$params.<field> or, on a form page, $form.<slot>)");
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
    auto checkListenMs = [&](const google::protobuf::int32 ms, const std::string& where) {
        if (ms < -1) check.error(where, "follow_up_listen_ms must be -1 (never close), 0 or more");
    };
    auto checkExpect = [&](const vasdk::flow::Expectation& expect, const std::string& where) {
        for (const auto& intent : expect.intents())
            if (!compiled->knowsIntent(intent)) check.error(where + " expect", "unknown intent " + quoted(intent));
        if (expect.has_min_digits() && expect.has_max_digits() && expect.min_digits() > expect.max_digits())
            check.error(where + " expect", "min_digits is more than max_digits");
        if (expect.has_no_input_ms() && expect.no_input_ms() < 0) check.error(where + " expect", "no_input_ms is negative");
        if (expect.has_max_reprompts() && expect.max_reprompts() < 0) check.error(where + " expect", "max_reprompts is negative");
        if (expect.has_follow_up_listen_ms()) checkListenMs(expect.follow_up_listen_ms(), where + " expect");
        for (const auto& text : expect.partial_reprompt()) checkTemplate(text, where + " expect partial_reprompt");
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
            if (slot.list() && slot.entity().empty() && slot.type() != vasdk::goal::Slot::ENUM)
                check.error(slotWhere, "list needs an entity or ENUM values");
            if (slot.skippable() && slot.optional()) check.error(slotWhere, "is optional, so never asked: skippable has no effect");
            checkExpect(slot.expect(), slotWhere);
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
        if (say.has_follow_up_listen_ms()) checkListenMs(say.follow_up_listen_ms(), at);
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
                checkExpect(step.ask().expect(), stepWhere);
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
            case vasdk::flow::Step::kCallTool: {
                const auto& call = step.call_tool();
                if (call.name().empty()) check.error(stepWhere, "call_tool has no name");
                else if (!extensions.tool(call.name()) && !declaredTools.count(call.name()))
                    check.error(stepWhere, "unknown tool " + quoted(call.name()) + " (declare it in tools, or FlowRegistry::addTool)");
                for (const auto& [arg, expression] : call.args())
                    compileExpression(expression, stepWhere + " call_tool arg " + quoted(arg));
                if (!call.into().empty()) checkWritable(call.into(), stepWhere, "call_tool into");
                for (const auto& text : call.filler()) checkTemplate(text, stepWhere + " filler");
                break;
            }
            case vasdk::flow::Step::kLlm:
                if (step.llm().instructions().empty()) check.error(stepWhere, "llm has no instructions");
                for (const auto& target : step.llm().allowed_targets()) checkTarget(target, stepWhere + " llm allowed_targets");
                checkSteps(step.llm().offline(), stepWhere + " offline");
                break;
            case vasdk::flow::Step::kCustom:
                if (readParams(step.custom(), stepWhere) && !extensions.step(typeName(step.custom())))
                    check.error(stepWhere, "no step is registered for " + quoted(typeName(step.custom())) +
                                               " (FlowRegistry::addStep)");
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
            else if (!route.intent().empty() && !compiled->knowsIntent(route.intent()))
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
            if (handler.event() != kEventNoMatch && handler.event() != kEventNoInput && handler.event() != kEventMaxReprompts &&
                handler.event() != kEventToolError && handler.event() != kEventClarify)
                check.error(handlerWhere, "unknown event " + quoted(handler.event()) +
                                              " (no-match, no-input, max-reprompts, tool-error or clarify)");
            checkSteps(handler.steps(), handlerWhere);
            checkTarget(handler.target(), handlerWhere);
        }
    };
    auto checkEnvelopeNames = [&](const google::protobuf::RepeatedPtrField<std::string>& names, const std::string& where) {
        for (const auto& name : names) {
            usedEnvelopes.insert(name);
            if (!compiled->_envelopes.count(name) && !extensions.envelope(name))
                check.error(where, "unknown envelope " + quoted(name) + " (declare it in envelope_defs, or register it with FlowRegistry::addEnvelope)");
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
        if (envelope.settings().has_follow_up_listen_ms()) checkListenMs(envelope.settings().follow_up_listen_ms(), where);
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
            // Its type and params: the type's steps are checked on each page that uses it.
            paramsType = nullptr;
            const auto declared = pageTypes.find(page.type());
            const auto* declaredType = declared == pageTypes.end() ? nullptr : declared->second;
            const auto* registeredType = declaredType || page.type().empty() ? nullptr : extensions.pageType(page.type());
            const std::string wanted = declaredType ? declaredType->params_type() : registeredType ? registeredType->paramsType : "";
            if (!page.type().empty() && !declaredType && !registeredType) {
                check.error(where, "unknown page type " + quoted(page.type()) +
                                       " (declare it in page_types, register it with FlowRegistry::addPageType, or use menu, yes_no or end)");
            } else if (page.has_params() && page.type().empty()) {
                check.error(where, "has params but no type");
            } else if (page.has_params() && wanted.empty()) {
                check.error(where, "page type " + quoted(page.type()) + " takes no params");
            } else if (!wanted.empty() && !page.has_params()) {
                check.error(where, "page type " + quoted(page.type()) + " needs params of type " + quoted(wanted));
            } else if (!wanted.empty() && typeName(page.params()) != wanted) {
                check.error(where, "params are " + quoted(typeName(page.params())) + ", but page type " + quoted(page.type()) +
                                       " takes " + quoted(wanted));
            } else if (const auto* params = page.has_params() ? readParams(page.params(), where + " params") : nullptr) {
                paramsType = params->GetDescriptor();
                if (registeredType && registeredType->targets)
                    for (const auto& target : registeredType->targets(*params)) checkTarget(target, where + " params");
            }
            if (declaredType) {
                const std::string typeWhere = where + " (page type " + quoted(page.type()) + ")";
                checkSteps(declaredType->on_enter(), typeWhere + " on_enter");
                checkRoutes(declaredType->routes(), typeWhere);
                checkHandlers(declaredType->handlers(), typeWhere);
            }
            checkSteps(page.on_enter(), where + " on_enter");
            checkRoutes(page.routes(), where);
            checkHandlers(page.handlers(), where);
            const bool leavesOnEntry = std::any_of(page.on_enter().begin(), page.on_enter().end(), [](const auto& step) {
                return step.has_goto_() || step.has_end();
            });
            const bool sharesRoutes = !page.envelopes().empty() || !flow.envelopes().empty() || !definition.envelopes().empty();
            if (page.routes().empty() && !leavesOnEntry && !page.has_form() && !sharesRoutes && page.type().empty())
                check.warn(where, "has no routes: every answer on it is not understood");
            const bool hasConditionRoute = std::any_of(page.routes().begin(), page.routes().end(), [](const auto& route) {
                return route.intent().empty() && route.entity().empty();
            });
            if (page.has_form() && !hasConditionRoute)
                check.warn(where, "has a form but no condition route: nothing happens once it is complete");
        }
    }
    formGoal = nullptr;
    paramsType = nullptr;
    for (const auto& envelope : definition.envelope_defs())
        if (!envelope.name().empty() && !usedEnvelopes.count(envelope.name())) {
            bool extended = false;
            for (const auto& other : definition.envelope_defs()) extended = extended || other.extends() == envelope.name();
            if (!extended) check.warn("envelope " + quoted(envelope.name()), "is not applied anywhere");
        }

    // Each page's layers, innermost first: its own, then its envelopes, its flow's and the agent's,
    // each followed by what it extends.
    // A page type's routes and handlers come right after the page's own; envelopes written in C++
    // are kept apart, in the same order.
    if (check.errors.empty()) {
        compiled->_layers.resize(definition.flows_size());
        compiled->_extensions.resize(definition.flows_size());
        for (int f = 0; f < definition.flows_size(); ++f) {
            const auto& flow = definition.flows(f);
            for (const auto& page : flow.pages()) {
                PageExtensions pageExtensions;
                std::vector<Layer> layers{{&page.routes(), &page.handlers(), nullptr}};
                if (const auto declared = pageTypes.find(page.type()); declared != pageTypes.end()) {
                    pageExtensions.declaredType = declared->second;
                    layers.push_back({&declared->second->routes(), &declared->second->handlers(), nullptr});
                } else if (!page.type().empty()) {
                    pageExtensions.registeredType = extensions.pageType(page.type());
                }
                if (page.has_params()) {
                    pageExtensions.params = compiled->_params.at(&page.params()).get();
                    google::protobuf::json::PrintOptions options;
                    options.preserve_proto_field_names = true;
                    std::string json;
                    if (!google::protobuf::util::MessageToJsonString(*pageExtensions.params, &json, options).ok() ||
                        !google::protobuf::util::JsonStringToMessage(json, &pageExtensions.paramsValue).ok())
                        spdlog::warn("[CompiledAgent] page {}: $params can't be read", page.name());
                }
                std::set<std::string> seen;
                auto add = [&](const google::protobuf::RepeatedPtrField<std::string>& names) {
                    for (const auto& name : names)
                        for (std::string at = name; !at.empty() && seen.insert(at).second;) {
                            if (!compiled->_envelopes.count(at)) {
                                pageExtensions.envelopes.push_back(extensions.envelope(at));
                                break;
                            }
                            const auto& envelope = definition.envelope_defs(compiled->_envelopes.at(at));
                            layers.push_back({&envelope.routes(), &envelope.handlers(), &envelope.settings()});
                            at = envelope.extends();
                        }
                };
                add(page.envelopes());
                add(flow.envelopes());
                add(definition.envelopes());
                compiled->_layers[f].push_back(std::move(layers));
                compiled->_extensions[f].push_back(std::move(pageExtensions));
            }
        }
    }

    // Triggers, and the proactive policy.
    formGoal = nullptr;
    paramsType = nullptr;
    auto validTime = [](const std::string& text) {
        return text.size() == 5 && text[2] == ':' && std::isdigit(static_cast<unsigned char>(text[0])) &&
               std::isdigit(static_cast<unsigned char>(text[1])) && std::isdigit(static_cast<unsigned char>(text[3])) &&
               std::isdigit(static_cast<unsigned char>(text[4])) && std::stoi(text.substr(0, 2)) < 24 && std::stoi(text.substr(3)) < 60;
    };
    const auto& policy = definition.proactive_policy();
    if (policy.quiet_from().empty() != policy.quiet_until().empty())
        check.error("proactive_policy", "quiet_from and quiet_until go together");
    for (const auto* time : {&policy.quiet_from(), &policy.quiet_until()})
        if (!time->empty() && !validTime(*time)) check.error("proactive_policy", quoted(*time) + " must be HH:MM, 00:00 to 23:59");
    if (policy.max_per_hour() < 0) check.error("proactive_policy", "max_per_hour is negative");
    std::set<std::string> triggerNames;
    for (int i = 0; i < definition.triggers_size(); ++i) {
        const auto& trigger = definition.triggers(i);
        const std::string where = "trigger " + (trigger.name().empty() ? std::to_string(i + 1) : quoted(trigger.name()));
        if (trigger.name().empty()) check.error(where, "has no name");
        else if (!triggerNames.insert(trigger.name()).second) check.error(where, "is declared twice");
        if (trigger.source().kind_case() == vasdk::flow::TriggerSource::KIND_NOT_SET) check.error(where, "has no source");
        if (trigger.source().has_inactivity() && trigger.source().inactivity().after_ms() <= 0)
            check.error(where, "inactivity after_ms must be more than 0");
        if (trigger.source().has_schedule()) {
            const auto& schedule = trigger.source().schedule();
            if (schedule.at().empty() == (schedule.every_minutes() <= 0))
                check.error(where, "schedule needs either at (\"08:00\", \"2026-11-09T08:00\") or every_minutes");
            else if (!schedule.at().empty()) checkTemplate(schedule.at(), where + " schedule at");
        }
        if (trigger.source().has_context_change()) compileExpression(trigger.source().context_change().condition(),
                                                                     where + " context_change condition");
        if (!trigger.condition().empty()) compileExpression(trigger.condition(), where + " condition");
        const auto& delivery = trigger.delivery();
        if (!delivery.dedupe_key().empty()) compileExpression(delivery.dedupe_key(), where + " dedupe_key");
        if (delivery.expires_after_ms() < 0 || delivery.cooldown_ms() < 0)
            check.error(where, "expires_after_ms and cooldown_ms can't be negative");
        for (const auto& text : delivery.ask_first().text()) checkTemplate(text, where + " ask_first");
        if (delivery.has_follow_up_listen_ms()) checkListenMs(delivery.follow_up_listen_ms(), where);
        const auto& action = trigger.action();
        switch (action.kind_case()) {
        case vasdk::flow::TriggerAction::kStartFlow:
            if (auto entered = compiled->resolve("flow:" + action.start_flow(), compiled->_startPage)) reachedFromAnywhere.push_back(*entered);
            else check.error(where, "unknown flow " + quoted(action.start_flow()));
            break;
        case vasdk::flow::TriggerAction::kGoto:
            if (action.goto_() != kTargetEndSession && action.goto_() != kTargetEndFlow && action.goto_() != kTargetCurrentPage) {
                if (auto next = compiled->resolve(action.goto_(), compiled->_startPage)) reachedFromAnywhere.push_back(*next);
                else check.error(where, "unknown target " + quoted(action.goto_()));
            }
            break;
        case vasdk::flow::TriggerAction::kSay:
            if (action.say().text().empty()) check.error(where, "say has no text");
            for (const auto& text : action.say().text()) checkTemplate(text, where + " say");
            break;
        case vasdk::flow::TriggerAction::KIND_NOT_SET:
            check.error(where, "has no action");
            break;
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

const google::protobuf::Message* CompiledAgent::params(const google::protobuf::Any& params) const {
    const auto found = _params.find(&params);
    return found == _params.end() ? nullptr : found->second.get();
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
    case Value::kStructValue: return type == vasdk::flow::SessionParam::STRUCT;
    default: return false;
    }
}

bool CompiledAgent::knowsIntent(const std::string& name) const { return _rules && _rules->knows(name); }

} // namespace flow
} // namespace va
