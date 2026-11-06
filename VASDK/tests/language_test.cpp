// Languages as packs a deployer writes, not code: French (tests/data/languages/fr-FR.txtpb, a
// pack VASDK doesn't ship) read and said on a fixed clock (Monday 2026-11-02, 09:00): dates,
// times, numbers, durations, emails, phone digits, yes/no; how replies say them; an agent in
// French that understands English too; a pack from a file, from the registry, and packs that
// don't load; a language written in C++ for grammar a pack can't express (Dutch "half vier" is
// 3:30); back-channels in French; text folded for matching ("Décembre", "don’t").

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/Entities.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/Language.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

using namespace va;
using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

std::chrono::system_clock::time_point localTime(int year, int month, int day, int hour) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

const std::string kFrench = std::string(VASDK_TEST_DATA) + "/languages/fr-FR.txtpb";

// Dutch says the half hour before the next one: "half vier" (half four) is 3:30. A pack's fractions
// count from the hour before ("half past three"), so this is code: the pack's times, and these.
class Dutch : public nlu::Language {
public:
    explicit Dutch(vasdk::nlu::Language pack) : Language(std::move(pack)) {}

    std::vector<nlu::Found> times(const std::vector<std::string>& words) const override {
        auto found = Language::times(words);
        for (size_t i = 0; i + 1 < words.size(); ++i) {
            size_t length = 0;
            const auto hour = words[i] == "half" ? smallNumber(words, i + 1, length) : std::nullopt;
            if (!hour || *hour < 1 || *hour > 12) continue;
            char value[8];
            std::snprintf(value, sizeof(value), "%02d:30", *hour == 1 ? 12 : *hour - 1);
            found.push_back({value, spokenTime(value), i, 1 + length});
        }
        return found;
    }
};

std::shared_ptr<const CompiledAgent> compileText(const std::string& text, std::string* error,
                                                 std::shared_ptr<const FlowRegistry> registry = nullptr) {
    vasdk::flow::Agent definition;
    if (!google::protobuf::TextFormat::ParseFromString(text, &definition)) {
        *error = "doesn't parse";
        return nullptr;
    }
    return CompiledAgent::compile(definition, error, nullptr, std::move(registry));
}

} // namespace

int main() {
    const auto now = localTime(2026, 11, 2, 9);
    std::string error;
    const auto french = nlu::Language::load(kFrench, &error);
    check(french != nullptr, "the French pack loads: " + error);
    if (!french) return 1;
    const auto english = nlu::Language::builtIn();
    check(english->code() == "en-US" && english->pack().numbers().words_size() > 20, "VASDK's built-in pack is en-US");
    check(nlu::Language::compile(english->pack(), &error) != nullptr, "the built-in pack checks clean: " + error);

    // French values, from words and digits.
    const EntityCatalog catalog(std::make_shared<const nlu::Languages>(nlu::Languages({french})));
    auto value = [&](const std::string& entity, const std::string& text) {
        const auto match = catalog.find(entity, text, {}, now);
        return match ? toText(match->value) : std::string("(none)");
    };
    auto expect = [&](const std::string& entity, const std::string& text, const std::string& wanted) {
        const auto got = value(entity, text);
        check(got == wanted, entity + " \"" + text + "\" → " + got + ", expected " + wanted);
    };
    expect("sys.date", "demain", "2026-11-03");
    expect("sys.date", "après-demain", "2026-11-04");
    expect("sys.date", "mardi prochain", "2026-11-03");   // next_is_coming: the coming Tuesday
    expect("sys.date", "le 10 novembre", "2026-11-10");
    expect("sys.date", "le dix novembre", "2026-11-10");
    expect("sys.date", "le premier décembre", "2026-12-01");
    expect("sys.date", "le 3 Décembre", "2026-12-03");
    expect("sys.date", "dans deux semaines", "2026-11-16");
    expect("sys.time", "quinze heures trente", "15:30");
    expect("sys.time", "15h30", "15:30");
    expect("sys.time", "à midi", "12:00");
    expect("sys.time", "trois heures et quart", "03:15");
    expect("sys.time", "cinq heures moins le quart", "04:45");
    expect("sys.time", "cinq heures moins dix", "04:50");
    expect("sys.time", "trois heures de l'après-midi", "15:00");
    expect("sys.datetime", "demain à midi", "2026-11-03T12:00");
    expect("sys.integer", "vingt et un", "21");
    expect("sys.integer", "quatre-vingt-dix-sept", "97");
    expect("sys.integer", "soixante et onze", "71");
    expect("sys.integer", "deux cent cinquante", "250");
    expect("sys.integer", "mille deux cents", "1200");
    expect("sys.number", "trois virgule cinq", "3.5");
    expect("sys.duration", "une heure et demie", "PT1H30M");
    expect("sys.duration", "un quart d'heure", "PT15M");
    expect("sys.yes_no", "oui", "true");
    expect("sys.yes_no", "non merci", "false");
    expect("sys.email", "jean point dupont arobase gmail point com", "jean.dupont@gmail.com");
    expect("sys.phone", "zéro six, douze, trente-quatre, cinquante-six", "06123456");
    expect("sys.ordinal", "la deuxième", "2");
    check(value("sys.date", "tomorrow") == "(none)", "a French-only agent doesn't read English dates");

    // How a French reply says them.
    check(french->spokenDate("2026-11-10") == "mardi 10 novembre", "a date: " + french->spokenDate("2026-11-10"));
    check(french->spokenDate("2026-12-01") == "mardi 1er décembre", "the 1st: " + french->spokenDate("2026-12-01"));
    check(french->spokenTime("15:30") == "15 h 30" && french->spokenTime("09:00") == "9 heures" && french->spokenTime("12:00") == "midi",
          "times: " + french->spokenTime("15:30") + ", " + french->spokenTime("09:00"));
    check(french->spokenDateTime("2026-11-03T12:00") == "mardi 3 novembre à midi", "a date and time");
    check(french->spokenDuration("PT1H30M") == "1 heure et 30 minutes" && french->spokenDuration("PT2H") == "2 heures",
          "durations: " + french->spokenDuration("PT1H30M"));
    check(french->plural({"jour", "jours"}, 0) == "jour" && french->plural({"jour", "jours"}, 2) == "jours", "French plurals: 0 is singular");
    check(english->plural({"day", "days"}, 0) == "days", "English plurals: 0 is plural");
    check(french->spokenList({"a", "b", "c"}) == "a, b et c", "a list");
    check(french->speakable("Le 2026-11-10 à 15:30, au 01 23 45 67 89.") == "Le mardi 10 novembre à 15 h 30, au 01 23 45 67 89.",
          "speakable: " + french->speakable("Le 2026-11-10 à 15:30, au 01 23 45 67 89."));

    // An agent in French whose users may also speak English.
    const std::string agentText = R"agent(
        locale: "fr-FR"
        languages: "en-US"
        start_flow: "main"
        intents { name: "reserver" examples: ["je veux réserver", "réserver"] }
        goals { name: "visite" slots { name: "jour" type: DATE prompt: "Quel jour ?" } }
        flows { name: "main" start_page: "accueil"
          pages { name: "accueil" on_enter { say { text: "Bonjour." } }
                  routes { intent: "reserver" target: "visite" }
                  routes { intent: "sys.yes" steps { say { text: "Parfait." } } } }
          pages { name: "visite" form { goal: "visite" }
                  routes { condition: "$form.complete" steps { say { text: "À ${form.jour}, ${2|plural:'voiture':'voitures'}." } } target: "accueil" } } }
    )agent";
    std::string agentText2 = agentText;
    const auto at = agentText2.find("start_flow");
    agentText2.insert(at, "language_files: \"" + kFrench + "\"\n        ");
    const auto folder = std::filesystem::temp_directory_path() / "vasdk_language_test";
    std::filesystem::create_directories(folder);
    const auto agentPath = (folder / "agent.txtpb").string();
    std::ofstream(agentPath) << agentText2;
    auto agent = CompiledAgent::load(agentPath, &error);
    check(agent != nullptr, "an agent with language_files loads: " + error);
    if (agent) {
        check(agent->languages().speaking().code() == "fr-FR" && agent->languages().all().size() == 2, "it speaks French and hears English too");
        FlowRuntime conversation(agent);
        conversation.setClock([now] { return now; });
        conversation.start();
        auto reply = conversation.handleText("je ne veux pas réserver");
        check(reply.text.find("Désolé, je ne peux pas vous aider avec ça ici.") != std::string::npos,
              "a French negation, and the built-in reply in French: " + reply.text);
        reply = conversation.handleText("oui");
        check(reply.text.find("Parfait.") != std::string::npos, "sys.yes in French: " + reply.text);
        reply = conversation.handleText("yes");
        check(reply.text.find("Parfait.") != std::string::npos, "and in English: " + reply.text);
        reply = conversation.handleText("je veux réserver");
        check(reply.text.find("Quel jour ?") != std::string::npos, "the form asks: " + reply.text);
        reply = conversation.handleText("next tuesday");   // in English, said back in French
        check(reply.text.find("À mardi 10 novembre, 2 voitures.") != std::string::npos, "an English date, said in French: " + reply.text);
    }
    std::filesystem::remove_all(folder);

    // A pack from the registry, by its language ("fr-CA" → fr-FR); one that isn't there; packs that don't check.
    {
        auto registry = std::make_shared<FlowRegistry>();
        registry->addLanguage(french);
        auto canadian = compileText(R"(locale: "fr-CA" start_flow: "main" flows { name: "main" start_page: "p" pages { name: "p" } })",
                                    &error, registry);
        check(canadian && canadian->languages().speaking().code() == "fr-FR", "fr-CA uses the registry's fr-FR: " + error);
        check(!compileText(R"(locale: "de-DE" start_flow: "main" flows { name: "main" start_page: "p" pages { name: "p" } })", &error) &&
                  error.find("language \"de-DE\": has no pack") != std::string::npos,
              "a language with no pack doesn't load: " + error);
        check(!compileText(R"(locale: "xx" language_defs { code: "xx" calendar { weekdays { phrases: "one" } } }
                               start_flow: "main" flows { name: "main" start_page: "p" pages { name: "p" } })", &error) &&
                  error.find("7 weekdays") != std::string::npos,
              "a pack with one weekday: " + error);
        check(!compileText(R"(locale: "xx" language_defs { code: "xx" speaking { plural_rules: "$n =" } }
                               start_flow: "main" flows { name: "main" start_page: "p" pages { name: "p" } })", &error) &&
                  error.find("plural_rules") != std::string::npos,
              "a plural rule that doesn't parse: " + error);
        auto plain = compileText(R"(start_flow: "main" flows { name: "main" start_page: "p" pages { name: "p" } })", &error);
        check(plain && plain->languages().speaking().code() == "en-US", "no language: VASDK's en-US");
    }

    // A language written in C++, added to the registry: the agent's time slot reads "half vier".
    {
        vasdk::nlu::Language pack;
        google::protobuf::TextFormat::ParseFromString(R"(
            code: "nl-NL" name: "Dutch"
            numbers { words { key: "drie" value: 3 } words { key: "vier" value: 4 } words { key: "tien" value: 10 } }
            speaking { time: "{hour}:{minute}" whole_hour: "{hour} uur" }
            replies { not_understood: "Sorry, daar kan ik u hier niet mee helpen." })", &pack);
        auto registry = std::make_shared<FlowRegistry>();
        registry->addLanguage(std::make_shared<Dutch>(pack));
        auto dutch = compileText(R"(locale: "nl-NL" start_flow: "main"
            goals { name: "afspraak" slots { name: "tijd" type: TIME prompt: "Hoe laat?" } }
            flows { name: "main" start_page: "p" pages { name: "p" form { goal: "afspraak" }
              routes { condition: "$form.complete" steps { say { text: "Om $form.tijd." } } } } })", &error, registry);
        check(dutch != nullptr, "an agent in a language written in C++: " + error);
        if (dutch) {
            FlowRuntime conversation(dutch);
            conversation.setClock([now] { return now; });
            conversation.start();
            const auto reply = conversation.handleText("half vier");
            check(reply.text.find("Om 3:30.") != std::string::npos, "\"half vier\" is 3:30: " + reply.text);
        }
    }

    // Back-channels in French: "ouais" lets the agent go on, "attendez" stops it.
    {
        pipeline::BackchannelConfig config;
        pipeline::useLanguageWords(config, nlu::Languages({french}));
        check(pipeline::classifyBackchannel("Ouais, d'accord.", true, config) == pipeline::BackchannelVerdict::Backchannel, "ouais");
        check(pipeline::classifyBackchannel("Attendez.", true, config) == pipeline::BackchannelVerdict::Interrupt, "attendez");
    }

    // Text as words are compared.
    check(nlu::foldedText("ÉTÉ Déjà ŒUVRE Ärger") == "été déjà œuvre ärger", "accented capitals: " + nlu::foldedText("ÉTÉ Déjà ŒUVRE Ärger"));
    check(nlu::foldedText("ПРИВЕТ Ωμέγα") == "привет ωμέγα", "Cyrillic and Greek");
    check(nlu::foldedText("Don’t") == "don't", "a typographic apostrophe");

    if (failures == 0) std::printf("language_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
