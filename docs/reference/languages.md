# Languages

A worked example, from a pack to a tested French agent: [An agent in another language](../examples/languages.md).

The engine has no English words in its code. Everything it reads or says in a language comes from a **language pack**, a `vasdk.nlu.Language` message ([`language.proto`](../../VASDK/proto/vasdk/nlu/language.proto)) written as a text file:

- the words for numbers, dates, times, durations and email addresses (`@sys.date`, `@sys.integer`...)
- yes and no, the built-in intents (`sys.yes`, `sys.cancel`...) and negations ("I don't want to book")
- back-channels ("mm-hmm") and the words that always interrupt the agent
- the replies the engine says itself ("Sorry, I didn't get that.")
- how values are said: "Tuesday, November 10th", "3:30 PM", "1 hour and 30 minutes", "a, b and c", plurals

VASDK ships one pack, [`languages/en-US.txtpb`](../../VASDK/languages/en-US.txtpb), built into the library. An agent that names no language uses it. Any other language, or a change to the English one, is a pack you write.

## An agent's languages

```
locale: "fr-FR"                          # the agent's texts and replies are in French
languages: ["en-US"]                     # its users may also speak English
language_files: ["languages/fr-FR.txtpb"]
```

| Field | What it does |
|---|---|
| `locale` | The language the agent's own texts are in. Replies say values in it, and the engine's own replies are in it. Empty: the first of `languages`, else `en-US`. |
| `languages` | Other languages the agent's users may speak. Values, yes and no, the built-in intents and back-channels are understood in all of them at once: "demain" and "tomorrow" both fill a date slot, said back in the locale's language. |
| `language_files` | Packs, relative to the agent file. Included files may list their own. |
| `language_defs` | Packs written inline in the agent. |

Each code finds its pack in this order:

1. the agent's own packs (`language_files`, `language_defs`)
2. packs the app added in code (see below)
3. VASDK's built-in `en-US`

An exact code wins first; otherwise a pack of the same language serves ("fr-CA" uses "fr-FR" when there's no "fr-CA"). A code with no pack fails the agent's load. So does a pack that doesn't check: 6 weekdays, an expression that doesn't parse, a unit with the wrong number of plural forms.

An agent's texts are written in its locale, one language per agent. Translating `say` texts isn't covered yet.

## Writing a pack

Copy `en-US.txtpb` and change it; `language_test.cpp` uses [a French pack](../../VASDK/tests/data/languages/fr-FR.txtpb) written that way. Words are matched as the recognizer's text is split: lower case (accented capitals too: "Décembre"), punctuation dropped, an apostrophe kept ("aujourd'hui"), a hyphen splitting words ("après-demain" is "après demain"). An entry may be a phrase of several words. Where phrases overlap, the longest one that fits wins.

| Section | Holds |
|---|---|
| `numbers` | Number words with their values, scales (`hundred`, `mille`), connectors (`and`, `et`), the decimal point word, digit aliases (`oh`), repeats (`double`), ordinals and their written suffix (`st\|nd\|rd\|th`). |
| `calendar` | 7 weekdays (Monday first) and 12 months, each with its spellings. Days from today ("tomorrow": 1, "day after tomorrow": 2), "in" + days or weeks, "next" and "this" around a weekday, "of" between day and month. |
| `clock` | noon, midnight, am and pm phrases, "at", hour words (`o'clock`, `heures`, `h`), fractions before the hour ("half past"), phrases after it ("et quart": 15, "moins le quart": -15), `minus` ("cinq heures moins dix"). |
| `durations` | Units in seconds, amounts before a unit ("half an": 0.5), "and a half". |
| `emails` | The word for @ and for each symbol ("dot", "point"). |
| `yes`, `no` | One-word answers for `@sys.yes_no` and yes/no questions. The first of each is how a reply says the answer. |
| `intents` | Examples for `sys.yes`, `sys.no`, `sys.cancel`, `sys.repeat`, `sys.help`, `sys.skip`. |
| `negations`, `articles`, `stop_words` | Words that negate what follows, articles before a day or a choice ("the fifth"), words ignored when knowledge questions are matched by words. |
| `spelling_alphabet` | Spelled letters for `spelled` patterns ("alpha" → a). |
| `turn_taking` | Back-channels and interruptions. The engine config's `turn_taking.backchannel` lists replace them. |
| `replies` | The engine's own replies: not understood, a tool failed, a slot's answer not read, "say yes or no", the slot question (`{slot}`), too few digits (`{count}`, one form per plural). |
| `speaking` | Formats with placeholders: date (`{weekday}, {month} {day_ordinal}`), time (`{hour}:{minute} {period}`), whole hour, date and time. Also 12 or 24 hours, am and pm, ordinal suffixes, plural rules, duration units, list separators, and digit names for phone numbers read one by one. |

**Numbers.** Words follow each other while each fills a smaller place: "twenty" "five" is 25, while "five" "five" is two numbers. A scale multiplies what comes before it: "two hundred and five" is 205. Where a language builds numbers some other way, list the phrase with its value: "soixante dix": 70, "quatre vingt": 80. A language that writes compounds as one word lists each one ("einundzwanzig": 21). Phone numbers can be said digit by digit or in pairs ("zéro six, douze, trente-quatre").

**Rules.** Plural forms and ordinal suffixes are expressions over `$n`, in the flow language. English has one rule, `"$n = 1"`, so 2 forms: "1 day", "2 days", "0 days". French has `"$n < 2"`, so 0 is singular. A language with 3 forms has 2 rules:

```
plural_rules: ["$n % 10 = 1 and $n % 100 != 11", "$n % 10 >= 2 and $n % 10 <= 4 and ($n % 100 < 12 or $n % 100 > 14)"]
ordinal_suffixes { condition: "$n = 1" suffix: "er" }   # "1er décembre"
```

A template's `|plural` filter takes as many forms as the reply's language has: `${$form.days|plural:'jour':'jours'}`.

## Adding packs from code

The app can add packs agents may name, for every agent it loads with that registry:

| From | How |
|---|---|
| C++ | `registry->addLanguage(va::nlu::Language::load("fr-FR.txtpb", &error))`. For grammar a pack can't express, subclass `va::nlu::Language`, override what differs (`dates`, `times`, `spokenDate`...), and add an instance. |
| C | `vasdk_registry_add_language(registry, pack, length, VASDK_FORMAT_TEXT, &error)`; `vasdk_language_builtin()` gives the en-US pack. |
| Python | `registry.add_language("fr-FR.txtpb")` or a `vasdk.Language` message; `vasdk.builtin_language()` returns the en-US pack to start from. |

A pack added with a code already added replaces it. An agent's own packs still win.

## What isn't per language yet

- **Agent texts.** An agent's `say` texts are in its locale, one language per agent.
- **Phonetic matching.** `matching { phonetic: true }` uses Double Metaphone, which models English spelling.
- **Phone grouping.** Text for a voice reads a 3-3-4 phone number in groups ("555-123-4567"). Other groupings are read as they are.
- **Recognizers and voices.** Their own language settings (Whistle, Phonon, Deepgram...) are provider options in the engine config, set apart from the agent's.
- **LLM instructions.** The engine's instructions to an LLM are English; the agent's own instructions are whatever it writes.
