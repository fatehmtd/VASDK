// Entities: list values with synonyms, spoken forms and attributes; dynamic values per
// conversation; patterns (spaces stripped, spelled out); derived entities (number ranges,
// conditions); the system entities; resolution between overlapping entities; lint errors; and
// every place an agent uses them: entity routes and $entity, $nlu.entities, form slots, extract,
// $entities, and test cases.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/flow/FlowTestRunner.h>

#include <google/protobuf/text_format.h>

#include <VASDK/flow/FlowRegistry.h>
#include <VASDK/nlu/NluProvider.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kAgent = R"agent(
start_flow: "main"
intents { name: "book" examples: ["book"] }
session_params { name: "plate" type: STRING }
entity_types {
  name: "service"
  list {
    values { value: "oil_change" spoken: "an oil change" synonyms: ["oil change", "change the oil", "lube"]
             attributes { fields { key: "price" value { number_value: 49 } } } }
    values { value: "tire_repair" spoken: "a tire repair" synonyms: ["tire repair", "flat tire", "tyres", "puncture"]
             attributes { fields { key: "price" value { number_value: 89 } } } }
  }
}
entity_types { name: "part" list { values { value: "tire" } values { value: "battery" } } }
entity_types { name: "plate" pattern { regex: "[a-z]{2}[0-9]{3}" spelled: true uppercase: true } }
entity_types { name: "code" pattern { regex: "[a-z]{2} [0-9]{3}" } }
entity_types { name: "party_size" derived { base: "@sys.integer" min: 1 max: 12 } }
entity_types { name: "cheap_service" derived { base: "@service" condition: "$value != 'tire_repair'" } }
entity_types { name: "time" dynamic: true list { values { value: "09:00" spoken: "9 AM" synonyms: ["nine", "9"] } } }
goals {
  name: "booking"
  require_confirmation: true
  confirm: "$form.service for $form.service.price dollars at $form.time. Right?"
  slots { name: "service" entity: "@service" prompt: "Which service?" }
  slots { name: "time" entity: "@time" prompt: "We have $entities.time free. Which time?" }
}
flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Hi." } } }
    routes { entity: "@service" condition: "$entity = 'tire_repair'" steps { say { text: "Ouch, $entity." } } target: "booking" }
    routes { intent: "book" target: "booking" }
    routes { entity: "@plate" steps { extract { entity: "@plate" into: "$session.plate" } }
             steps { say { text: "Plate $session.plate." } } }
    routes { condition: "exists($nlu.entities.service)" steps { say { text: "That costs $nlu.entities.service.price dollars." } } }
  }
  pages {
    name: "booking"
    form { goal: "booking" }
    routes { condition: "$form.confirmed" target: "done" }
  }
  pages { name: "done" on_enter { say { text: "Done." } } }
}
)agent";

std::shared_ptr<const CompiledAgent> compileText(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error);
}

std::string lintError(const std::string& replace, const std::string& with) {
    std::string text = kAgent;
    const auto at = text.find(replace);
    if (at == std::string::npos) return "test bug: '" + replace + "' not found";
    text.replace(at, replace.size(), with);
    std::string error;
    return compileText(text, &error) ? "" : error;
}

std::string found(const CompiledAgent& agent, const std::string& entity, const std::string& text,
                  const DynamicEntityValues& dynamic = {}) {
    const auto match = agent.entities().find(entity, text, dynamic);
    return match ? toText(match->value) : "-";
}

vasdk::nlu::EntityValue entityValue(const std::string& value, const std::string& spoken, const std::string& synonym) {
    vasdk::nlu::EntityValue entity;
    entity.set_value(value);
    entity.set_spoken(spoken);
    entity.add_synonyms(synonym);
    return entity;
}

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the entity agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    const auto& entities = agent->entities();

    // List entities
    check(found(*agent, "service", "can you change the oil?") == "oil_change", "a synonym");
    check(found(*agent, "service", "I've got a PUNCTURE") == "tire_repair", "any case");
    check(found(*agent, "service", "a car wash") == "-", "nothing");
    const auto oil = entities.find("service", "a lube please");
    check(oil && oil->spoken == "an oil change" && oil->raw == "lube" && oil->attributes.fields().at("price").number_value() == 49,
          "spoken form, words heard and attributes");

    // Dynamic values replace the declared ones for one conversation.
    const DynamicEntityValues today{{"time", {entityValue("14:00", "2 PM", "two")}}};
    check(found(*agent, "time", "nine works") == "09:00" && found(*agent, "time", "nine works", today) == "-" &&
              found(*agent, "time", "two please", today) == "14:00", "dynamic values");

    // Patterns
    check(found(*agent, "plate", "it's alpha bravo one two three") == "AB123", "spelled out, upper case");
    check(found(*agent, "plate", "ab 123") == "AB123", "letters and digits as the STT writes them");
    check(found(*agent, "code", "the code is xy 789 thanks") == "xy 789", "a pattern across words");

    // Derived
    check(found(*agent, "party_size", "a table for six") == "6" && found(*agent, "party_size", "for forty") == "-",
          "a number range");
    check(found(*agent, "cheap_service", "an oil change") == "oil_change" && found(*agent, "cheap_service", "a flat tire") == "-",
          "a condition on the value");

    // System entities
    check(found(*agent, "sys.integer", "twenty five") == "25" && found(*agent, "sys.number", "2.5 liters") == "2.5",
          "numbers");
    check(found(*agent, "sys.integer", "five five") == "5", "repeated digits are separate numbers");
    check(found(*agent, "sys.phone", "call 555 123 4567 today") == "5551234567" && found(*agent, "sys.phone", "five five") == "-",
          "a phone number");
    check(found(*agent, "sys.yes_no", "yeah go on") == "true", "yes");

    // Resolution: longest first, then pattern over list, then the first declared.
    auto all = entities.findAll("fix a flat tire and the battery");
    check(all.size() == 2 && all[0].entity == "service" && toText(all[0].value) == "tire_repair" && all[1].entity == "part",
          "the longest match wins over a shorter one inside it");

    // Lint
    check(contains(lintError("synonyms: [\"tire repair\", \"flat tire\"", "synonyms: [\"tire repair\", \"lube\""),
                   "\"lube\" is a synonym of both"), "a synonym of two values");
    check(contains(lintError("[a-z]{2}[0-9]{3}", "[a-z{2"), "regex"), "a bad regex");
    check(contains(lintError("base: \"@sys.integer\"", "base: \"@sys.colour\""), "base \"@sys.colour\""), "an unknown base");
    check(contains(lintError("$value != 'tire_repair'", "$session.plate != 'x'"), "it can read only $value"),
          "a derived condition reads only $value");
    check(contains(lintError("name: \"code\" pattern", "name: \"code\" dynamic: true pattern"), "only list entities can be dynamic"),
          "dynamic patterns");
    check(contains(lintError("entity: \"@time\"", "entity: \"@times\""), "unknown entity \"@times\""), "a slot's unknown entity");
    check(contains(lintError("routes { entity: \"@plate\"", "routes { entity: \"@plates\""), "unknown entity \"@plates\""),
          "a route's unknown entity");
    check(contains(lintError("say { text: \"Done.\" }", "say { text: \"Done $entity.\" }"), "$entity is read only in a route"),
          "$entity outside an entity route");
    check(contains(lintError("$nlu.entities.service.price", "$nlu.entities.sevice.price"), "unknown reference"),
          "$nlu.entities of an unknown entity");

    // Entity routes: $entity in the condition and the reply (spoken there).
    FlowRuntime conversation(agent);
    conversation.start();
    auto reply = conversation.handleText("I've got a flat tire");
    check(conversation.pageName() == "booking" && contains(reply.text, "Ouch, a tire repair."), "an entity route with $entity");
    // The service from the way in fills the form; the prompt lists the dynamic entity's values.
    check(contains(reply.text, "We have 9 AM free. Which time?") && toText(conversation.lookup({"form", "service"})) == "tire_repair",
          "an entity slot stores the value; $entities lists the spoken values");
    reply = conversation.handleText("nine please");
    check(reply.text == "a tire repair for 89 dollars at 9 AM. Right?", "spoken values and attributes in the read-back");

    // Not the route's condition: the other routes are tried.
    FlowRuntime other(agent);
    other.start();
    reply = other.handleText("how much is a lube");
    check(other.pageName() == "welcome" && reply.text == "That costs 49 dollars. Hi.", "$nlu.entities and its attribute");
    reply = other.handleText("my plate is alpha bravo one two three");
    check(reply.text == "Plate AB123. Hi." && toText(other.sessionParam("plate")) == "AB123", "extract into a session param");

    // Dynamic values from the app
    FlowRuntime later(agent);
    check(later.setEntityValues("time", {entityValue("15:30", "half past three", "half past three")}), "set dynamic values");
    check(!later.setEntityValues("service", {}, &error) && contains(error, "not a dynamic"), "only dynamic entities");
    later.start();
    reply = later.handleText("book an oil change");
    check(contains(reply.text, "We have half past three free."), "the replaced values are offered");
    reply = later.handleText("nine");
    check(!reply.matched && toText(later.lookup({"form", "time"})).empty(), "the declared values no longer match");
    reply = later.handleText("half past three");
    check(contains(reply.text, "an oil change for 49 dollars at half past three"), "the replaced value fills the slot");

    // Test cases set dynamic values and check $nlu.entities.
    vasdk::flow::FlowTestCase testCase;
    google::protobuf::TextFormat::ParseFromString(R"(
        entities { name: "time" values { value: "10:00" spoken: "10 AM" synonyms: "ten" } }
        turns { user: "book a lube" expect { page: "booking" said_contains: "We have 10 AM free." entities { key: "service" value: "oil_change" } } }
        turns { user: "ten" expect { said_contains: "at 10 AM" } }
    )", &testCase);
    const auto result = runFlowTest(agent, testCase);
    check(result.passed(), "a test case with dynamic values");
    for (const auto& failure : result.failures) std::fprintf(stderr, "  %s\n", failure.c_str());

    // Version 2: composite entities, built from others; names matched by how they sound.
    {
        std::string error;
        auto cars = compileText(R"agent(
            start_flow: "main"
            entity_types { name: "color" list { values { value: "white" } values { value: "red" } } }
            entity_types { name: "make" list { values { value: "toyota" spoken: "Toyota" } values { value: "honda" spoken: "Honda" } } }
            entity_types { name: "car" composite { templates: ["@color:color @make:make", "@color:color @make:make from @sys.integer:year"] } }
            entity_types { name: "contact" list { values { value: "katherine" spoken: "Katherine" } values { value: "stephen" spoken: "Stephen" } }
                           matching { phonetic: true } }
            flows { name: "main" start_page: "p" pages { name: "p" on_enter { end {} } } }
        )agent", &error);
        check(cars != nullptr, "the v2 agent compiles");
        if (cars) {
            const auto car = cars->entities().find("car", "it's a white toyota from 2019");
            check(car && car->value.struct_value().fields().at("make").string_value() == "toyota" &&
                      car->value.struct_value().fields().at("year").number_value() == 2019 && car->spoken == "white Toyota from 2019",
                  "a composite: the longest template, a field per part");
            const auto all = cars->entities().findAll("a red honda please");
            check(all.size() == 1 && all[0].entity == "car", "a composite beats its parts");
            check(cars->entities().find("contact", "call catherine")->value.string_value() == "katherine", "sounds alike: Catherine");
            check(cars->entities().find("contact", "message steven")->value.string_value() == "stephen", "sounds alike: Steven");
            check(!cars->entities().find("contact", "call kevin"), "a different name isn't matched");
        }
        check(contains(lintError("entity_types {", "entity_types { name: \"bad\" composite { templates: [\"@nothing:x\"] } }\nentity_types {"),
                       "@nothing must be"),
              "a composite's part must be an entity");
    }

    // Double Metaphone: a primary code, and an alternate for spellings with two common sounds.
    {
        check(va::nlu::doubleMetaphone("Smith") == std::make_pair(std::string("SM0"), std::string("XMT")), "Smith: SM0, XMT");
        check(va::nlu::doubleMetaphone("Schmidt") == std::make_pair(std::string("XMT"), std::string("SMT")), "Schmidt: XMT, SMT");
        check(va::nlu::doubleMetaphone("Thompson").first == "TMPS", "Thompson: TMPS (th as t)");
        check(va::nlu::doubleMetaphone("knight").first == "NT", "knight: the k and gh are silent");
        check(va::nlu::doubleMetaphone("Philip").first == va::nlu::doubleMetaphone("Filip").first, "ph sounds like f");
        check(va::nlu::soundsAlike("Smith", "Schmidt") && va::nlu::soundsAlike("Jon", "John") &&
                  va::nlu::soundsAlike("Catherine", "Kathryn") && !va::nlu::soundsAlike("Kevin", "Katherine"),
              "sounds alike by either code");
        std::string error;
        auto people = compileText(R"agent(
            start_flow: "main"
            entity_types { name: "person" list { values { value: "smith" spoken: "Ms Smith" } values { value: "al" spoken: "Al" } }
                           matching { phonetic: true } }
            flows { name: "main" start_page: "p" pages { name: "p" on_enter { end {} } } }
        )agent", &error);
        check(people && people->entities().find("person", "put me through to schmidt")->value.string_value() == "smith",
              "a name spelled another way is found by sound");
        check(people && !people->entities().find("person", "is that all"), "short words aren't matched by sound");
    }

    // Code-backed entities: the app's extractor finds the values.
    {
        auto registry = std::make_shared<FlowRegistry>();
        registry->addEntityExtractor("plates_db", [](const FlowRegistry::EntityRequest& request) {
            std::vector<FlowRegistry::EntityFound> found;
            for (size_t i = 0; i < request.words.size(); ++i) {
                if (request.words[i] != "golf") continue;
                FlowRegistry::EntityFound car;
                car.start = i;
                car.value = stringValue("AB123CD");
                car.spoken = "your Golf";
                (*car.attributes.mutable_fields())["model"] = stringValue("Golf");
                found.push_back(car);
                FlowRegistry::EntityFound outside;   // past the words: dropped
                outside.start = request.words.size();
                outside.value = stringValue("nowhere");
                found.push_back(outside);
            }
            return found;
        });
        vasdk::flow::Agent definition;
        google::protobuf::TextFormat::ParseFromString(R"agent(
            start_flow: "main"
            entity_types { name: "my_car" code { extractor: "plates_db" } }
            flows { name: "main" start_page: "p" pages { name: "p" on_enter { end {} } } }
        )agent", &definition);
        std::string error;
        auto agent = CompiledAgent::compile(definition, &error, nullptr, registry);
        check(agent != nullptr, "a code entity compiles with its extractor registered");
        if (agent) {
            const auto car = agent->entities().find("my_car", "the golf needs a wash");
            check(car && car->value.string_value() == "AB123CD" && car->spoken == "your Golf" &&
                      car->attributes.fields().at("model").string_value() == "Golf" && car->raw == "golf",
                  "found by the extractor, with its spoken form and attributes");
            check(!agent->entities().find("my_car", "the polo needs a wash"), "nothing found, no match");
        }
        check(!CompiledAgent::compile(definition, &error) && contains(error, "plates_db"),
              "an extractor that isn't registered fails to load");
    }

    // Entity packs: shared files of entity types, and VASDK's own (country, us_state, currency).
    {
        const auto dir = std::filesystem::temp_directory_path() / "vasdk_entity_packs";
        std::filesystem::create_directories(dir);
        const auto packs = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path().parent_path() / "entities");
        std::ofstream(dir / "agent.txtpb") << "start_flow: \"main\"\n"
                                           << "entity_files: [\"" << (packs / "country.txtpb").generic_string() << "\", \""
                                           << (packs / "us_state.txtpb").generic_string() << "\", \""
                                           << (packs / "currency.txtpb").generic_string() << "\"]\n"
                                           << "entity_types { name: \"currency\" list { values { value: \"coins\" } } }\n"
                                           << "flows { name: \"main\" start_page: \"p\" pages { name: \"p\" on_enter { end {} } } }\n";
        std::string error;
        auto agent = CompiledAgent::load((dir / "agent.txtpb").string(), &error);
        check(agent != nullptr, "an agent with entity packs loads");
        if (agent) {
            const auto country = agent->entities().find("country", "I'm flying to the netherlands next week");
            check(country && country->value.string_value() == "netherlands" && country->attributes.fields().at("iso2").string_value() == "NL",
                  "a country, with its ISO code");
            const auto state = agent->entities().find("us_state", "we moved to new york");
            check(state && state->value.string_value() == "new_york" && state->attributes.fields().at("code").string_value() == "NY",
                  "a state, with its postal code");
            check(!agent->entities().find("us_state", "or maybe in a week"), "postal codes that are words aren't states");
            check(agent->entities().find("currency", "a few coins") && !agent->entities().find("currency", "ten euros"),
                  "a type the agent declares itself wins over the pack's");
        }
        std::ofstream(dir / "broken.txtpb") << "start_flow: \"main\" entity_files: [\"missing.txtpb\"]\n";
        check(!CompiledAgent::load((dir / "broken.txtpb").string(), &error) && contains(error, "missing.txtpb"),
              "a missing pack fails to load, naming it");
        std::filesystem::remove_all(dir);
    }

    if (failures == 0) std::printf("entity_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
