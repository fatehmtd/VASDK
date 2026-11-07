# Example: an agent in another language

This walks through a garage booking agent in French, whose callers may also speak English. It needs no code: a language pack file, the agent, and a test. Then it shows how an app adds a language from C++, C or Python, and how C++ covers grammar a pack can't express. The reference is [Languages](../reference/languages.md).

## 1. The language pack

Start from VASDK's own [`en-US.txtpb`](../../VASDK/languages/en-US.txtpb) and translate each section. The French pack used here is [`VASDK/tests/data/languages/fr-FR.txtpb`](../../VASDK/tests/data/languages/fr-FR.txtpb). A few of its entries:

```
code: "fr-FR"
name: "French"

numbers {
  words { key: "vingt" value: 20 }
  words { key: "soixante dix" value: 70 }       # what "each word fills a smaller place" doesn't cover
  words { key: "quatre vingt" value: 80 }
  scales { key: "cent" value: 100 }
  connectors: "et"                               # "vingt et un"
  ordinals { key: "premier" value: 1 }
  ordinal_suffix: "er|re|e|ème|eme"              # "1er", "2e"
}
calendar {
  weekdays { phrases: "lundi" }                  # 7, Monday first
  months { phrases: ["août", "aout"] }           # 12, January first; the first is how it's said
  relative_days { key: "demain" value: 1 }
  relative_days { key: "après demain" value: 2 }
  next: ["prochain", "prochaine"]
  next_is_coming: true                           # "mardi prochain" is the coming Tuesday
}
clock {
  noon: "midi"
  hour_words: ["heure", "heures", "h"]           # "quinze heures trente", "15h30"
  after_hour { key: "et quart" value: 15 }
  after_hour { key: "moins le quart" value: -15 }
  minus: "moins"                                 # "cinq heures moins dix"
}
yes: ["oui", "ouais", "d'accord"]
intents { key: "sys.yes" value { phrases: ["oui", "d'accord", "c'est ça"] } }
negations: ["ne", "n'", "pas", "jamais"]
replies {
  not_understood: "Désolé, je ne peux pas vous aider avec ça ici."
  ask_slot: "Quel est votre {slot} ?"
}
speaking {
  date: "{weekday} {day_ordinal} {month}"        # "mardi 1er décembre"
  ordinal_suffixes { condition: "$n = 1" suffix: "er" }
  time: "{hour} h {minute}"                      # 24 hours: twelve_hour is false
  whole_hour: "{hour} heures"
  plural_rules: "$n < 2"                         # 0 and 1 are singular
  units { key: "day" value { phrases: ["jour", "jours"] } }
  list_last: " et "
}
```

## 2. The agent

Next to the pack, `agent.txtpb`:

```
# proto-file: vasdk/flow/agent.proto
# proto-message: vasdk.flow.Agent
schema_version: 1
name: "garage-fr"
locale: "fr-FR"                    # its texts and replies are in French
languages: ["en-US"]               # its callers may speak English too
language_files: ["fr-FR.txtpb"]
start_flow: "main"

intents { name: "reserver" examples: ["réserver", "prendre rendez-vous"] }
goals {
  name: "rdv"
  slots { name: "jour" type: DATE prompt: "Pour quel jour ?" }
  slots { name: "heure" type: TIME prompt: "À quelle heure ?" }
}
flows { name: "main" start_page: "accueil"
  pages { name: "accueil" on_enter { ask { say { text: "Bonjour, que puis-je faire pour vous ?" } } }
          routes { intent: "reserver" target: "rdv" } }
  pages { name: "rdv" form { goal: "rdv" }
          routes { condition: "$form.complete"
                   steps { say { text: "C'est noté : $form.jour à $form.heure, dans ${days_until($form.jour)|plural:'jour':'jours'}." } }
                   target: "accueil" } } }
```

The agent's texts are French. The pack covers the rest:
- the dates and times it reads, in French and English;
- how it says them back ("mardi 10 novembre", "15 h 30");
- the plural (`|plural` picks "jour" or "jours" by the French rule);
- its own replies when it doesn't understand.

## 3. Check it, try it, test it

```
vasdk_agentc lint agent.txtpb
# agent.txtpb: ok (1 flows, 2 pages, 1 intents, 0 entities)

vasdk_agentc try agent.txtpb --entity @sys.date "le premier décembre"
# @sys.date = 2026-12-01  spoken "mardi 1er décembre"  heard "le premier décembre"

vasdk_agentc try agent.txtpb "je ne veux pas réserver"
# intent: (none)                       (the French negation)

vasdk_agentc chat agent.txtpb
# you: je voudrais réserver
# bot: Pour quel jour ?
# you: le 2 août
# bot: À quelle heure ?
# you: à midi
# bot: C'est noté : lundi 2 août à midi, dans 270 jours. Bonjour, que puis-je faire pour vous ?
```

A test case, `tests/reserver.txtpb`, on a fixed clock, mixing the two languages:

```
# proto-file: vasdk/flow/test_case.proto
# proto-message: vasdk.flow.FlowTestCase
name: "a booking, in French and in English"
start_time: "2026-11-02T09:00:00"
turns { user: "je voudrais prendre rendez-vous" expect { page: "rdv" said: "Pour quel jour ?" } }
turns { user: "next tuesday" expect { said: "À quelle heure ?" } }
turns { user: "quinze heures trente" expect { said_contains: "C'est noté : mardi 10 novembre à 15 h 30, dans 8 jours." } }
```

```
vasdk_agentc test agent.txtpb tests/reserver.txtpb
# PASS a booking, in French and in English
# 1 of 1 passed
```

"next tuesday" is read by the en-US pack and said back by the French one. With a `languages` list, each value is looked for in every language, and the longest match wins.

In a session, set the recognizer's and the voice's own `language` options to French too (the engine config's provider `options`). The pack decides what the agent reads and says, not what the recognizer hears.

## 4. Adding a language from the app

An app can add packs for every agent it loads, instead of each agent listing a file. A pack the agent lists itself still wins.

C++:

```cpp
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/nlu/Language.h>

auto registry = std::make_shared<va::flow::FlowRegistry>();
std::string error;
auto french = va::nlu::Language::load("languages/fr-FR.txtpb", &error);   // nullptr, with the reasons, if it doesn't check
if (!french) spdlog::error(error);
registry->addLanguage(french);
auto agent = va::flow::CompiledAgent::load("agent.txtpb", &error, nullptr, registry);   // locale: "fr-CA" finds fr-FR too
```

C:

```c
const char* pack = "code: \"es\" name: \"Spanish\" yes: [\"sí\"] no: [\"no\"]\n"
                   "replies { not_understood: \"Lo siento, no puedo ayudarle con eso.\" }";
char* error = NULL;
if (!vasdk_registry_add_language(registry, pack, strlen(pack), VASDK_FORMAT_TEXT, &error)) {
    fprintf(stderr, "%s\n", error);   /* "language \"es\": plural_rules ..." */
    vasdk_free(error);
}
size_t length = 0;
char* english = vasdk_language_builtin(VASDK_FORMAT_TEXT, &length);   /* en-US, to start one from */
vasdk_free(english);
```

Python, from the built-in pack:

```python
import vasdk

spanish = vasdk.builtin_language()            # a vasdk.Language message: en-US
spanish.code, spanish.name = "es", "Spanish"
spanish.replies.not_understood = "Lo siento, no puedo ayudarle con eso."
del spanish.intents["sys.yes"].phrases[:]
spanish.intents["sys.yes"].phrases.extend(["sí", "claro"])

registry = vasdk.Registry()
registry.add_language(spanish)                # or a file: registry.add_language("es.txtpb")
agent = vasdk.Agent.load("agent.txtpb", registry)   # locale: "es-MX"
```

On Android, put the pack beside `agent.txtpb` in the agent's assets folder and list it in `language_files`.

These snippets run in VASDK's tests: [`language_test.cpp`](../../VASDK/tests/language_test.cpp), [`vasdk_c_test.c`](../../VASDK/tests/vasdk_c_test.c) and [`test_extensions.py`](../../bindings/python/tests/test_extensions.py).

## 5. Grammar a pack can't express

Dutch says the half hour before the next one: "half vier" (half four) is 3:30. A pack's fractions count from the hour before ("half past three"), so this takes code. Subclass `va::nlu::Language`, call the pack's version, and add what it misses:

```cpp
class Dutch : public va::nlu::Language {
public:
    explicit Dutch(vasdk::nlu::Language pack) : Language(std::move(pack)) {}

    std::vector<va::nlu::Found> times(const std::vector<std::string>& words) const override {
        auto found = Language::times(words);   // what the pack reads: "drie uur", "15:30"
        for (size_t i = 0; i + 1 < words.size(); ++i) {
            size_t length = 0;
            const auto hour = words[i] == "half" ? smallNumber(words, i + 1, length) : std::nullopt;
            if (!hour || *hour < 1 || *hour > 12) continue;
            char value[8];
            std::snprintf(value, sizeof(value), "%02d:30", *hour == 1 ? 12 : *hour - 1);
            found.push_back({value, spokenTime(value), i, 1 + length});   // value, as said, the words it took
        }
        return found;
    }
};

registry->addLanguage(std::make_shared<Dutch>(pack));   // pack: a vasdk::nlu::Language read from nl-NL.txtpb
```

Every method a value goes through is virtual, both for reading (`dates`, `times`, `numbers`, `yesNo`...) and for saying (`spokenDate`, `spokenTime`, `spokenList`, `pluralForm`, `speakable`). `smallNumber` and `ordinalNumber` read the pack's number words for you. Words arrive split by `flow::entityWords()`, lower case.
