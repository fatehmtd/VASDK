# Example: device controls in an agent

This walks through voice commands for a device: the phone's flashlight, the car's volume and heat. They're written in the agent with patterns and phrase sets rather than left to an LLM. It covers when a phrase set, an entity or a pack fits, how an agent of several files keeps its wordings apart, and how each piece is tested. The reference is [Agents: Patterns and phrase sets](../reference/flows.md#patterns-and-phrase-sets). The complete agents are the Android app's [phone agent](../../VASDK_ANDROID/app/src/main/assets/agents/phone/agent.txtpb) and VASDK_Car's [media](../../VASDK_CAR_APP/verticals/media/media.txtpb) and [climate](../../VASDK_CAR_APP/verticals/climate/climate.txtpb) parts.

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

## 5. A phrase set, an entity, or a pack?

| You want | Use | Because |
|---|---|---|
| Ways of saying the same thing: "turn on", "switch on", "power on" | a phrase set | Text only, spelled where it's written, no value kept |
| To know which one was said: which contact, which room's light | an entity (`@contact`) | It recognises the value, misspelled or not, and `$entity` holds it |
| Values that change while the app runs: the phone book, the stations | a dynamic list entity | `setEntityValues` replaces them |
| The same wording in several files | a pack | One file, each part says it uses it |
| A wording one intent needs differently | the intent's own `phrase_sets` | It hides the file's set of the same name, for that intent only (lint notes it) |

"The lights" on a phone is a phrase set: there's one light to switch. In a smart home, where "the kitchen light" and "the bedroom light" are different things, it's an entity.

## 6. Testing it

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
