# The conversation loop: entities, flows, pages and turns

Steps 9 and 10 of [the setup wizard](SKILL.md). The user describes the conversation, you write it into the agent file one piece at a time, `lint` checks each piece, and you keep asking until they say they're done. The reference is [Agents](../../docs/reference/flows.md); the schema is `VASDK/proto/vasdk/flow/agent.proto`; the worked example is `VASDK_APP/agents/garage-menu/`.

Before the first piece, write the agent's frame if it's new: the `# proto-file: vasdk/flow/agent.proto` and `# proto-message: vasdk.flow.Agent` lines, `schema_version: 1`, `name`, `locale` (and `languages`, `language_files`) from step 1, and `start_flow: "main"`.

## 9. Entities

**Tell the user what they are**, in about these words: an entity is a kind of value people say (a service, a date, a plate number). It's declared once and used by name, `@service`, in what the agent listens for, in the forms that collect values, and in what it says back. Conditions read its `value` (`oil_change`); replies say its `spoken` form ("an oil change").

**Ask which values callers will name**, one at a time, and for each offer what's already there before anything custom.

### Ready-made

| | What | How it's used |
| --- | --- | --- |
| System entities | `@sys.integer`, `@sys.number`, `@sys.phone`, `@sys.digits`, `@sys.yes_no`, `@sys.date`, `@sys.time`, `@sys.datetime`, `@sys.duration`, `@sys.email`, `@sys.ordinal` ([what each reads and says](../../docs/reference/flows.md#entities)) | nothing to declare: name it |
| Entity packs | the files in `VASDK/entities/` (list them when you ask): `@country` (attributes `iso2`, `iso3`), `@us_state` (`code`), `@currency` (`code`) | `entity_files: ["<relative path to the pack>"]` in the agent |

A pack's values are snake_case names (`united_kingdom`, `us_dollar`); its codes are attributes, read as `$form.country.iso2`.

### Custom

Written in the agent's `entity_types` (schema `VASDK/proto/vasdk/nlu/entity.proto`). Ask the kind, then its attributes:

| Kind | For | Ask for |
| --- | --- | --- |
| `list` | a fixed set of values | each `value`, its `synonyms` (how people and speech recognizers say it), `spoken`, and `attributes` (price, duration: `$form.service.price`). `dynamic: true` if the app replaces the values per conversation (`Session::setEntityValues`). `matching`: `fuzzy`, `min_similarity` (0.8), `phonetic` (for names), `label` (what an entity model is told it is) |
| `pattern` | codes, IDs, plates | `regex` (ECMAScript, ignores case), `strip_spaces`, `spelled` ("alpha bravo one" → `ab1`), `uppercase` |
| `derived` | another entity, narrowed | `base` (`@sys.integer`, `@sys.number`, a list or a pattern), `min`, `max`, `condition` on `$value` |
| `composite` | one value made of parts | `templates`, each part `@entity:field` with the words between them as said |
| `code` | values only the app knows (a database, contacts) | `extractor`, which the app registers: `FlowRegistry::addEntityExtractor` in C++, `@registry.entity_extractor` in Python (step 3) |

Show the matching example when you ask:

```textproto
entity_types {
  name: "service"
  list {
    values { value: "oil_change" spoken: "an oil change" synonyms: ["oil change", "lube"]
             attributes { fields { key: "price" value { number_value: 49 } } } }
    values { value: "tire_repair" spoken: "a tire repair" synonyms: ["tire", "tyres", "flat", "puncture"] }
  }
  matching { min_similarity: 0.85 label: "car repair" }
}
entity_types { name: "plate"      pattern { regex: "[a-z]{2}[0-9]{3}[a-z]{2}" spelled: true uppercase: true } }
entity_types { name: "party_size" derived { base: "@sys.integer" min: 1 max: 12 } }
entity_types { name: "vehicle"    composite { templates: ["@color:color @car_make:make from @sys.integer:year"] } }
entity_types { name: "contact"    code { extractor: "contacts" } }
```

- **With an entity model** (step 5's `entity_model`), misspellings and sound-alikes count only inside the words the model marks; with `gliner`, ask each list entity's `label`, since that's all it's told.
- **Shared between agents:** offer an `EntityPack` file of its own (`name`, `entity_types`) listed in `entity_files`, instead of writing it in the agent.
- **After each entity:** write it, add a line with it to `nlu_eval.txtpb` (`cases { text: "…" intent: "…" entities { key: "service" value: "oil_change" } }`), and run `vasdk_agentc lint`. It catches a synonym shared by two values, a regex that doesn't compile, a bad derived base and an unknown `@name`.

Then ask: another entity, or on to the conversation?

## 10. Flows, pages and turns

**Tell the user how a conversation is built:**
- A **flow** is a topic: booking, prices, a handoff. `flow:<name>` enters it; `END_FLOW` goes back to the page that left, so a question about prices mid-booking comes back to the booking.
- A **page** is one point in a flow. Entering it runs its `on_enter` steps (say something, ask a question with choices), then it waits for the user.
- A **turn** is what the user says on a page. The page's routes are tried: those with an intent first, then those with only an entity, then those with only a condition. The matching route runs its steps and moves to its `target`. Nothing matching is a `no-match`; silence is a `no-input`.
- An **intent** is what the user wants, recognised from examples (`"book"`, `"book an @service for @sys.date"`). `sys.yes`, `sys.no`, `sys.cancel`, `sys.repeat`, `sys.help` and `sys.skip` are built in.

### The flows loop

Propose `main` as the start flow. For each flow: its name, and one line on what it's for, written as a `#` comment above it. Then "Another flow?" until they say no. A flow named as a target later and not written yet goes on the to-do list.

### The pages loop, per flow

For each page, ask in this order:

1. **Name and purpose**, one line, written as a `#` comment above the page.
2. **Kind:**

   | Kind | Written as | Ask for |
   | --- | --- | --- |
   | plain | `on_enter` steps and routes | what it says, its turns |
   | menu | `type: "menu"`, `MenuParams` | the prompt; each option's `label`, `synonyms`, `target` |
   | yes or no | `type: "yes_no"`, `YesNoParams` | the `question`, `yes_target`, `no_target` |
   | form | `form { goal: "<name>" }` and `goals/<name>.txtpb` | each slot: `name`, `entity` (or a `type`: ENUM, INTEGER, PHONE…), `prompt`, `reprompt`, `sensitive`; `require_confirmation` and `confirm`; what happens on `$form.confirmed` and `$form.failed` ([forms](../../docs/reference/flows.md#forms)) |
   | LLM | `llm { instructions allowed_targets }` in a route or `on_enter` | its instructions, where it may go next ([with an LLM](../../docs/reference/flows.md#with-an-llm)) |
   | end | `type: "end"`, `EndParams` | the goodbye `text` |

3. **What it says on entry:** `say { text }` (several texts are said in turn), or `ask { say { text } choices { label value } }` when it expects an answer.
4. **Its turns.** Ask "When the user says…, what happens?" and turn each answer into a route:
   - *what triggers it:* an intent (reuse one or make one with its examples), an entity (`entity: "@service"`, optionally `condition: "$entity = 'tire_repair'"`), or a condition alone (`"$session.tier = 'gold'"`);
   - *what it does:* steps: `say`, `ask`, `set` (`$session.<name>`, declared in `session_params`), `extract { entity into }`, `call_tool`, `branch`, `llm`, `end`;
   - *where it goes:* a page of this flow, `flow:<name>`, `END_FLOW`, `END_SESSION`, or nowhere (stay and ask again).

   "Another turn on this page?" until they say no.
5. **When nothing fits:** optional `no-match` and `no-input` handlers; behaviour every page shares (hours, a person, goodbye) is better as an envelope ([shared behaviour](../../docs/reference/flows.md#shared-behaviour-and-navigation)).

A page written from these answers:

```textproto
# Asks what the caller needs, and books it or answers a price.
pages {
  name: "welcome"
  on_enter { ask { say { text: "Hi, this is Acme Garage. Book a service, or hear a price?" }
                   choices { label: "Book a service" value: "book" } } }
  routes { intent: "book" target: "booking" }
  routes { intent: "price" entity: "@service"
           steps { say { text: "$entity costs $entity.price dollars." } } }
  routes { entity: "@service" target: "booking" }   # "my tyres are flat"
}
```

**Offer the entities at every point a value fits:** in intent examples (`"book an @service"`), as a route's `entity`, as form slots, in `extract`, and in replies (`$form.service`, `$form.service.price`, `$entity.price`, `$entities.available_time` for every value of a list). When the user wants a value no entity reads, go back to step 9 for it, then return to the same question.

**After each page:**
1. Write it into the agent (new intents and `session_params` with it).
2. Run `vasdk_agentc lint`; fix errors with the user before going on.
3. Add `tests/<page>.txtpb`, a `FlowTestCase` with a turn per route ([checking and testing](../../docs/reference/flows.md#checking-and-testing-vasdk_agentc)):

   ```textproto
   # proto-file: vasdk/flow/test_case.proto
   # proto-message: vasdk.flow.FlowTestCase
   name: "welcome: naming a service starts a booking"
   turns { expect { page: "welcome" expects_reply: true } }
   turns { user: "my tyres are flat" expect { page: "booking" } }
   ```
4. Ask "Another page?" Offer the pages on the to-do list first (targets named but not written). Repeat until they say no.

### Until they're satisfied

When the to-do list is empty and they've declined another page, run `vasdk_agentc test` on all the cases, then ask:
- **Try it** by text: `vasdk_agentc chat <agent>` (or `<name>_agentc`), and go through what surprised them.
- **Add or change** a flow, a page or a turn: back to the loop above, at that point.
- **Add or change** an entity: back to step 9, then here again.
- **Done**: on to step 11 of [the wizard](SKILL.md).

Ask again after each change, until they pick done.

### Changing an existing agent

Read the agent (and its `include_files`, `goal_files`, `entity_files`) and show it as a tree: flows → pages → routes (intent or entity → target), and the entities with their kinds. The user picks what to add, change or delete; ask only that piece's questions, edit in place keeping the comments, then lint and test as above. Deleting a page also offers to remove the routes that target it and its test file.
