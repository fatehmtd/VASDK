# Example: device controls in an agent

This walks through voice commands for a device: the phone's flashlight, the car's volume and heat. They're written in the agent with patterns and phrase sets rather than left to an LLM. It covers when a phrase set, an entity or a pack fits, how an agent of several files keeps its wordings apart, wildcards for words nobody can list, the volume as a number with bounds and the screen's brightness as an enum, and how each piece is tested. The reference is [Agents: Patterns and phrase sets](../reference/flows.md#patterns-and-phrase-sets). The complete agents are the Android app's [phone agent](../../VASDK_ANDROID/app/src/main/assets/agents/phone/agent.txtpb) and VASDK_Car's [media](../../VASDK_CAR_APP/verticals/media/media.txtpb) and [climate](../../VASDK_CAR_APP/verticals/climate/climate.txtpb) parts.

## Why commands belong in the agent

A small on-device LLM is unreliable at device commands. On a phone, LFM2.5-230M often answered "turn on the flashlight" in words instead of calling its tool, and once switched it on because of a sentence it overheard. An agent's route matches the words and calls the tool every time, at once, with no model. What the agent doesn't understand still goes on to the LLM.

The cost is wording. Matching by examples means saying every way people ask: "turn on the flashlight", "switch the torch on", "lights on"... Patterns and phrase sets keep that to a few lines.

## 1. One command, written as patterns

The flashlight, before:

```textproto
intents { name: "flashlight_on"
          examples: ["turn on the flashlight", "turn the flashlight on", "switch on the flashlight", "switch the flashlight on",
                     "flashlight on", "turn on the torch", "turn the torch on", "switch on the torch", "switch the torch on", "torch on"] }
```

and after, saying more with less:

```textproto
phrase_sets { name: "light" phrases: ["flashlight", "torch", "(light|lights)"] }
intents { name: "flashlight_on" examples: ["(turn|switch|put|power) on [the] ${light}", "(turn|switch|put) [the] ${light} on", "${light} on"] }
```

`(a|b)` is one of them, `[x]` is optional, and `${light}` is one of the `light` set's phrases. The three examples spell 60 sentences, from "turn on the flashlight" to "power on lights". To see them:

```
$ vasdk_agentc expand agents/phone/agent.txtpb flashlight_on
flashlight_on (60)
  turn on the flashlight
  turn on the torch
  …
```

The longest sentence found still wins, so a short, general intent like `"turn on"` loses to "turn on the light". A sentence that names nothing to switch, such as "turn on", isn't matched.

## 2. Sets built from sets

When on and off share their wording, put the verbs in sets of their own and build the commands from them:

```textproto
phrase_sets { name: "on"      phrases: ["(turn|switch|put|power) on"] }
phrase_sets { name: "off"     phrases: ["(turn|switch|put|power) off"] }
phrase_sets { name: "light"   phrases: ["flashlight", "torch", "(light|lights)"] }
phrase_sets { name: "on_cmd"  phrases: ["${on} [the] ${light}", "(turn|switch|put) [the] ${light} on", "${light} on"] }
phrase_sets { name: "off_cmd" phrases: ["${off} [the] ${light}", "(turn|switch|put) [the] ${light} off", "${light} off"] }
intents { name: "flashlight_on"  examples: ["${on_cmd}"] }
intents { name: "flashlight_off" examples: ["${off_cmd}"] }
```

A set's phrases are written as examples are, so they use other sets and the same syntax. Sets that use each other in a loop fail the load and name the loop.

## 3. A pack for the verbs every command uses

The verbs aren't about the flashlight. A pack holds them for every file that needs them:

```textproto
# phrases.txtpb, beside the agent
# proto-message: vasdk.flow.PhraseSetPack
name: "phone"
phrase_sets { name: "on"  phrases: ["(turn|switch|put|power) on"] }
phrase_sets { name: "off" phrases: ["(turn|switch|put|power) off"] }
```

The agent uses it with `phrase_set_files: ["phrases.txtpb"]`, and keeps what's specific to it (`light`, `on_cmd`, `off_cmd`) as its own sets. It writes `${on}`, or `${phone.on}` to say whose. VASDK ships a larger pack, [`phrases/en-US-devices.txtpb`](../../VASDK/phrases/en-US-devices.txtpb), with on and off, up and down, louder, quieter, brighter and dimmer. Name it by its path, or copy it next to the agent. The Android app copies its agent's folder out of the app's assets, so its pack has to sit there.

## 4. Several files, each its own wording

VASDK_Car is one agent built from ten files, one per vertical. Media and climate both have something going up: the volume, the heat. Each has a set named `up`, and they don't collide, because phrase sets are never merged across files. Each file's examples are expanded with its own sets before the files are merged.

```textproto
# verticals/media/media.txtpb: "up" is about the sound
phrase_set_files: ["../../agents/car/phrases.txtpb"]                 # the car's shared verbs: ${raise}, ${lower}...
phrase_sets { name: "sound" phrases: ["[the] (volume|music)"] }
phrase_sets { name: "up"    phrases: ["louder", "volume up", "${raise} ${sound}", "(turn|crank|pump) it up"] }
intents { name: "louder" examples: ["${up}"] }

# verticals/climate/climate.txtpb: "up" is about the heat
phrase_set_files: ["../../agents/car/phrases.txtpb"]
phrase_sets { name: "heat" phrases: ["the (heat|temperature)"] }
phrase_sets { name: "up"   phrases: ["[a bit] warmer [in here]", "(i'm|i am) cold", "${raise} ${heat}", ...] }
intents { name: "warmer" examples: ["${up}"] }
```

Where a `${name}` is looked up, nearest first: the intent's own sets, then its file's, then the packs its file uses. A part never sees another part's sets. The car's pack (`agents/car/phrases.txtpb`) is read once, though both parts use it.

Scoping keeps the sets apart, but two intents can still end up with the same sentence, for instance "turn it up" for both the volume and the heat. When both compete on a page, the first route takes it, which is rarely what was meant. Lint warns about it:

```
warning: intents "louder" (verticals/media/media.txtpb) and "warmer" (verticals/climate/climate.txtpb): both have
"turn it up", and compete on the same page: the better match wins, then the first route
```

`car_agentc expand agents/car/agent.txtpb --collisions` lists every one. A page route overriding an envelope's isn't one: it always wins, which is how a page takes a word for itself ("options" on the route list).

## 5. Words you can't list: wildcards

People put words where an example has none: "turn **my** light on", "switch **that** torch off", "set **cabin** temperature to 22". A wildcard stands for them:

```textproto
phrase_sets { name: "on_cmd" phrases: ["${on} [the] ${light}", "(turn|switch) _ ${light} on", "${light} on"] }   # phone
intents { name: "set_temperature" examples: ["set the temperature to @temperature", "set _ temperature to @temperature"] }   # car
```

| Wildcard | Stands for | Use it for |
|---|---|---|
| `_` | exactly one word | a word you can't predict: "my", "that", "cabin" |
| `+` | one or more words | a part that's never empty: "call **my brother** back" (`call + back`) |
| `*` | any number, none too | rarely: see below |

Wildcards count no words when examples are compared, so where a specific example and a loose one both fit, the specific one wins. A wildcard sentence never takes a turn a precise one was written for.

The pitfall is `*`. `"turn * on"` matches "turn on the radio", and also "turn off the radio and put the light on": it only asks for "turn" somewhere before "on". Prefer `_` (one word, so "turn the radio on" but not a whole clause), or a phrase set listing what may be there. Lint stops an example that's only wildcards (it would match anything), and warns about a `*` at an example's start or end (it adds nothing: examples are found anywhere in the words).

## 6. The volume: a number with bounds

"Louder", "quieter", "set the volume to 8": a value with a range. Declare the range on the param, and the steps that move it stay short:

```textproto
session_params { name: "audio"  type: STRUCT }   # get_volume's result: {"current": 7, "max": 15}
session_params { name: "volume" type: NUMBER min: "0" max: "$session.audio.max" step: "1" out_of_range: CLAMP }

routes { intent: "volume_up"
         steps { call_tool { name: "get_volume" into: "$session.audio" } }        # the phone's own, it has buttons too
         steps { set { key: "$session.volume" value: "$session.audio.current" } }
         steps { increase { key: "$session.volume" } }
         steps { call_tool { name: "set_volume" args { key: "level" value: "$session.volume" } } }
         steps { say { text: "Volume $session.volume." } } target: "idle" }
```

- **Bounds from the device.** `max: "$session.audio.max"` is read each time it's used: phones differ (15 steps on one, 25 on another), and the agent asks, rather than assuming.
- **At the bound.** "Louder" at 15 doesn't say "Volume 15." again. `increase` raises `at-limit`, skips the steps after it (so `set_volume` isn't called), and the page's handler answers:

```textproto
handlers { event: "at-limit"
           steps { branch { if: "$bounds.volume.at_max" then { say { text: "That's as loud as it goes." } }
                                                        else { say { text: "It's as quiet as it goes." } } } } target: "idle" }
```

- **A value too big.** What "set the volume to 50" does is the param's `out_of_range`, and the param chooses. Neither clamping nor rejecting is on unless asked:

| `out_of_range` | "set the volume to 50", max 15 | Good for |
|---|---|---|
| `ALLOW` (unset) | Stored as 50; the bounds only limit `increase` and `decrease` | Values a tool checks itself |
| `CLAMP` | Stored as 15: "Volume 15, as loud as it goes." (`$entity > $bounds.volume.max` tells you it was) | A volume, a brightness percentage: the nearest is what was meant |
| `REJECT` | Kept as it was; `out-of-range` is raised: "That goes from 0 to 15." | A thermostat, a timer: a wrong number deserves a question, not a guess |

A step that would go past a bound stops at it: from 14 with a step of 2 and a max of 15, "louder" is 15.

## 7. The brightness: levels, as an enum

The screen has levels people name: "dim", "half", "full", "a bit brighter". An `ENUM` param holds one of a list entity's values, in their order:

```textproto
entity_types { name: "brightness_level" matching { fuzzy: false } list {
  values { value: "dim"  synonyms: ["dark", "lowest"]           attributes { fields { key: "level" value { number_value: 10 } } } }
  values { value: "low"                                         attributes { fields { key: "level" value { number_value: 64 } } } }
  values { value: "mid"  synonyms: ["medium", "half", "normal"] attributes { fields { key: "level" value { number_value: 140 } } } }
  values { value: "high" synonyms: ["bright", "full", "maximum"] attributes { fields { key: "level" value { number_value: 255 } } } } } }
session_params { name: "brightness" type: ENUM entity: "@brightness_level" default { string_value: "mid" } }

routes { intent: "brightness_up"
         steps { increase { key: "$session.brightness" } }                        # mid → high; at high: at-limit
         steps { call_tool { name: "set_screen_brightness" args { key: "level" value: "$bounds.brightness.attributes.level" } } }
         steps { say { text: "Brightness $session.brightness." } } target: "idle" }
routes { intent: "set_brightness" entity: "@brightness_level"
         steps { set { key: "$session.brightness" value: "$entity" } }            # "dark" is stored as dim
         ... }
```

- **The levels' order is the values' order.** `increase` moves to the next, `decrease` to the previous, and `at-limit` is raised at either end. `wrap: true` goes round instead, for modes (a fan's auto, cool, heat; a repeat mode's off, all, one).
- **Each level's device setting is an attribute.** `$bounds.brightness.attributes.level` is 140 at mid, so the agent speaks of levels and the tool gets numbers.
- **Comparing levels:** `$bounds.brightness.index` is the level's place, from 0, and `index_of($bounds.brightness.values, 'mid')` is a named level's. `'mid' < 'high'` compares text, alphabetically, so it's no use for levels.
- **Fuzzy matching and synonyms.** With misspellings allowed, "brightness" is 80% like "brightest", so "set the brightness to half" read "brightness" as the value `high`. Synonyms close to the words of the commands themselves want `matching { fuzzy: false }`, as here.

## 8. A phrase set, an entity, or a pack?

| You want | Use | Because |
|---|---|---|
| Ways of saying the same thing: "turn on", "switch on", "power on" | a phrase set | Text only, spelled where it's written, no value kept |
| To know which one was said: which contact, which room's light | an entity (`@contact`) | It recognises the value, misspelled or not, and `$entity` holds it |
| Values that change while the app runs: the phone book, the stations | a dynamic list entity | `setEntityValues` replaces them |
| The same wording in several files | a pack | One file, each part says it uses it |
| A wording one intent needs differently | the intent's own `phrase_sets` | It hides the file's set of the same name, for that intent only (lint notes it) |
| A value with a range: volume, temperature | a NUMBER param with `min`, `max`, `step` | `increase`, `decrease`, `at-limit`; `out_of_range` for a value too big |
| Levels in an order: dim to high, a fan's speeds, modes | an ENUM param over a list entity | Synonyms understood, one level at a time, `attributes` for the device, `wrap` for modes |

"The lights" on a phone is a phrase set: there's one light to switch. In a smart home, where "the kitchen light" and "the bedroom light" are different things, it's an entity.

## 9. Testing it

- `vasdk_agentc lint` checks the patterns and the sets: unclosed brackets, unknown sets, loops, unused sets and packs, and the collisions above. Given a pack, it checks the pack alone.
- `vasdk_agentc expand` shows every sentence. Read it once: a pattern often spells something nobody says, or misses something everybody does.
- Test cases say the sentences people say, not the ones in the file:

```textproto
# agent-tests/phone/flashlight_phrasings.txtpb
turns { user: "Turn on the lights." tool_results { key: "set_flashlight" value: "{\"flashlight\": \"on\"}" }
        expect { said: "Flashlight on." tools_called: "set_flashlight" } }
turns { user: "put the light off please" tool_results { key: "set_flashlight" value: "{\"flashlight\": \"off\"}" }
        expect { said: "Flashlight off." tools_called: "set_flashlight" } }
turns { user: "turn on" expect { matched: false } }   # nothing to switch
```

- When rewriting existing examples as patterns, compare `expand` before and after: no sentence the old examples had should be lost. VASDK_Car's rewrite kept all 100 and gained 51.
