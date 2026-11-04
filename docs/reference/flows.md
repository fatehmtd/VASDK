# Agents

An agent file describes a conversation as pages: what the assistant says on each, which answers it understands, and where each answer leads. It runs with no LLM. With an LLM configured, the agent answers first and the LLM takes what no page understands.

```cpp
#include <VASDK/VAEngine.h>
#include <VASDK/flow/CompiledAgent.h>

std::string error;
va::VAEngine::Options options;
options.transport = transport;
options.agent = va::flow::CompiledAgent::load("agents/garage-menu/agent.txtpb", &error);
if (!options.agent) { spdlog::error(error); return 1; }   // every problem, with where it is
auto engine = va::VAEngine::create("engine.txtpb", registry, options, &error);
engine->start();
```

In the desktop app: `VASDK_Pipeline --config config/engine.txtpb --agent agents/garage-menu/agent.txtpb`.

## An agent file

Agents are text proto ([`agent.proto`](../../VASDK/proto/vasdk/flow/agent.proto)); JSON and binary load too. A small menu agent (the [example](../../VASDK_APP/agents/garage-menu/agent.txtpb) grows this with a form and entities):

```textproto
# proto-file: vasdk/flow/agent.proto
# proto-message: vasdk.flow.Agent
start_flow: "main"

intents { name: "book"  examples: ["book", "appointment", "bring my car in"] }
intents { name: "hours" examples: ["hours", "open"] }
intents { name: "tires" examples: ["tire", "tires", "flat"] }

flows {
  name: "main"
  start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "Hi, this is Acme Garage. Book a service, or hear our hours?" } } }
    routes { intent: "book" target: "service" }
    routes { intent: "hours" steps { say { text: "We're open 8 to 6, Monday to Saturday." } } }
  }
  pages {
    name: "service"
    on_enter { ask { say { text: ["Is it for tires?", "Tires, then?"] } } }
    routes { intent: "tires" steps { say { text: "Booked for tomorrow at 9. Goodbye!" } } target: "END_SESSION" }
  }
}
```

| Part | What it does |
|---|---|
| `start_flow` | The flow a conversation starts in, at its `start_page`. |
| `intents` | What the user wants, by keyword phrase: an intent matches when the user's words contain one of its `examples`, in order, as whole words ("book" doesn't match "bookkeeping"). Case and punctuation are ignored. |
| `flows` | Named groups of pages. `target: "flow:handoff"` goes to another flow's start page, remembering where it came from: `END_FLOW` goes back (see [Shared behaviour and navigation](#shared-behaviour-and-navigation)). |
| `pages` | `on_enter` steps run each time the conversation enters the page; `routes` are tried against each user turn while it's there. |
| `routes` | `intent` to match, `steps` to run, then `target`: a page of this flow, `flow:<name>`, `END_FLOW`, `CURRENT_PAGE` (enter this page again) or `END_SESSION`. No target: stay on the page without entering it again, and ask its question again unless the steps asked something. |

Steps:

| Step | Example | What it does |
|---|---|---|
| `say` | `say { text: ["Got it.", "Okay."] }` | Speaks one text; with several, a different one each time the step runs. Also `ssml`, `interruptible` and `ui`: see [Rich replies](#rich-replies). |
| `ask` | `ask { say { text: "Which day?" } }` | Speaks a question: the reply expects an answer (`agent_reply.expects_reply`), and the question is repeated when the answer isn't understood. `choices` offer answers as buttons. |
| `goto` | `goto { target: "confirm" }` | Moves to the target at once; the steps after it don't run. |
| `end` | `end {}` | Ends the conversation once the reply has played. |

Everything said in one turn is joined into one reply: a route's steps, then the `on_enter` steps of every page entered.

## Rich replies

A reply can carry more than its words:

```textproto
on_enter { say { text: "Welcome to Acme Garage." ui { fields { key: "card" value { string_value: "logo" } } } } }
on_enter { say { text: "Calls are recorded." interruptible: false } }
on_enter { ask { say { ssml: "<speak>Book a service <break time='300ms'/> or hear our hours?</speak>" }
                 choices { label: "Book a service" value: "book" synonyms: ["make a booking"] }
                 choices { label: "Opening hours" value: "hours" } } }
```

| Field | What it does |
|---|---|
| `interruptible: false` | The user can't talk over this part: it plays to its end. What they say meanwhile is kept and answered once the whole reply has played, like speech over a reply's last words. The parts before and after can still be interrupted. |
| `ssml` | Spoken by a voice provider that reads SSML (`supportsSsml()`). Other voices speak `text`, or the SSML with its tags removed when there's no `text`. References (`$form.day`) work in it. No bundled provider reads SSML yet. |
| `ui` | Any structure, given to the app with the reply (`agent_reply.ui_json`): a card, a map, a picture. |
| `choices` (on `ask`) | Answers the app can show as buttons (`agent_reply.choices`, with `choice_values`). A button sends its value as the user's words (`injectUserText`). Saying a label, value or synonym as the whole answer counts as saying the value: "Opening hours" routes like "hours". A choice without a value sends its label. Asked again, the question offers them again. |

**Was the reply heard?** When the user cuts a reply off, the flow is told. A read-back cut off before its end isn't confirmed by a plain "yes" in the next turn: it's read back again, so nobody confirms what they didn't hear. Any other answer (a new day, a "no") counts as usual.

**Checked at load:** a `say` or `ask` with neither text nor SSML, a choice without a label, and the references in SSML.

## How a turn is answered

1. **Matching.** Every route on the current page is scored by the longest example of its intent found in the user's words, if its `condition` holds. The best score wins; on a tie, the first route. "cancel my appointment" picks `cancel` (three words) over `book` ("appointment", one word). When no intent route matches, the first route with only a condition that holds is taken. Only when nothing on the page matches are its envelopes' routes tried, then the flow's and the agent's: a page route always wins over a shared one, however short its example.
2. **Not understood.** No route matched: the `no-match` handler runs, or by default the reply is "Sorry, I can't help with that here." and the page's last question. With an LLM, the turn goes to the LLM instead.
3. **Start.** The start page's `on_enter` (the greeting) runs once the assistant is listening (at start when always on, else on the wake word or button) and someone is connected to hear it: a browser peer over WebRTC, a caller. With the local mic that's at once. If the user speaks first, their words are routed on the start page without the greeting. When the peer or caller disconnects, the next one starts a new conversation with its own greeting.
4. **End.** After an `end` step or `END_SESSION`, the conversation starts over: the greeting plays on the next activation, or, when always on, the next turn is routed on the start page.
5. **Resumed turns.** When the user carries on talking after a turn was answered (`UserTurnFrame::replacesPrevious`), the agent goes back to where it was before that turn and routes the whole sentence.
6. **Loops.** A turn that enters more than 8 pages (a `goto` loop) stops on the page it reached, with an error in the log.

## Shared behaviour and navigation

**Envelopes** hold what many pages share: global commands, what to do when nothing matches or the user is silent, and settings. Declare them once and apply them by name to the whole agent, a flow or a page:

```textproto
envelopes: ["standard"]                 # every page
envelope_defs {
  name: "standard"
  routes { intent: "hours"  steps { say { text: "We're open 8 to 6." } } }   # then the page's question again
  routes { intent: "prices" target: "flow:prices" }                         # a digression: back afterwards
  routes { intent: "person" target: "flow:handoff" }
  handlers { event: "no-input"      steps { say { text: ["Are you still there?", "Hello?"] } } }
  handlers { event: "max-reprompts" steps { say { text: "Let me get someone." } } target: "flow:handoff" }
  settings { no_input_ms: 8000 max_reprompts: 2 }
}
envelope_defs { name: "booking" extends: "standard" settings { max_reprompts: 1 } }
flows { name: "booking" envelopes: ["booking"] … }   # a flow's pages; pages take `envelopes` too
```

**Layering.** For each turn, the page's own routes come first, then the page's envelopes, the flow's and the agent's, in the order listed, each followed by the envelope it `extends`. The first layer with a match wins. Handlers and settings are found the same way, so an inner one replaces an outer one. An envelope's targets can't name a page (it would be a different page wherever it's used): `flow:<name>`, `END_FLOW`, `END_SESSION` or `CURRENT_PAGE`.

**Handlers** replace the built-in behaviour for an event. Pages take `handlers` too:

| Event | When | Built in |
|---|---|---|
| `no-match` | No route understood the user | "Sorry, I can't help with that here." and the question again |
| `no-input` | Silence for `no_input_ms` after a question has played | The question again |
| `max-reprompts` | More than `max_reprompts` (default 2) no-match or no-input in a row on a page | The no-match or no-input behaviour again |

The handler's steps run, then it moves to its `target`. If it stays on the page and asked nothing, the page's question is asked again (on a form page, the form's). The no-input timer runs only when a `no-input` handler or `no_input_ms` applies to the page; it starts once the question has finished playing and stops when the user starts speaking. On a form page, answers that don't fit are re-asked by the form, and `max-reprompts` runs once the form fails and no `$form.failed` route takes it. `Reply::event` says which one ran; test cases check it and `vasdk_agentc chat` shows it.

**Flows as digressions.** `flow:<name>` remembers the page it left, with its form values and question. `END_FLOW` goes back there and asks that question again:

```
bot: Which day works for you?                [main/booking]
you: how much is an oil change               → flow:prices, which says the price, then END_FLOW
bot: An oil change costs 49 dollars. Which day works for you?   [main/booking]
```

Flows nest up to 8 deep; deeper, the way back isn't kept. `END_FLOW` in the start flow ends the conversation, and `END_SESSION` forgets every way back.

## Stored values and conditions

An agent can remember things and branch on them. Declare the values a conversation keeps:

```textproto
session_params { name: "tier"    type: STRING default { string_value: "standard" } }   # STRING, NUMBER, BOOL or LIST
session_params { name: "service" type: STRING }                                        # no default: null
```

Then set, test and say them:

```textproto
on_enter { branch { if: "$session.tier = 'gold'" then { say { text: "Welcome back, gold member." } } } }
routes { intent: "oil_change" steps { set { key: "$session.service" value: "'an oil change'" } } target: "booked" }
on_enter { say { text: "You're booked for $session.service tomorrow at 9 AM." } }
```

| Step | Example | What it does |
|---|---|---|
| `set` | `set { key: "$session.tier" value: "'gold'" }` | Stores the value of an expression in a session param or in `$page.locals.<name>` (kept until the conversation leaves the page). |
| `branch` | `branch { if: "$session.points > 100" then { … } else { … } }` | Runs `then` when the condition holds, else `else`. A `goto` or `end` inside stops the steps after the branch too. |
| `switch` | `switch { on: "$nlu.intent" cases { value: "book" steps { … } } default { … } }` | Runs the first case whose `value` equals the expression, else `default`. |

Routes take a `condition` too: `routes { intent: "hours" condition: "$context.closed_today = true" … }`.

**What can be referenced:**

| Reference | Value |
|---|---|
| `$session.<name>` | A declared session param. |
| `$page.name`, `$page.locals.<name>` | The current page, and values `set` on it. |
| `$input.text` | What the user said this turn. |
| `$nlu.intent` | The intent of the route that matched this turn. |
| `$context.<name>…` | App data from `Session::Options::context`: device state, location, anything the app's `ContextProvider` holds. |
| `$nlu.entities.<entity>`, `$entity`, `$entities.<entity>` | Entities in this turn's words, a route's entity, and every value of a list entity: see [Entities](#entities). |
| `$form.<slot>`, `$form.complete`, `$form.confirmed`, `$form.failed` | On a form page: see [Forms](#forms). |

Objects and lists are walked with `.field` and `[index]`: `$context.system.battery`, `$session.cars[0]`.

**Expressions** are strings, in the style of Dialogflow CX conditions:

| Form | Example |
|---|---|
| Compare: `=` `!=` `<` `<=` `>` `>=` | `$session.points >= 100`, `$session.tier != 'gold'` |
| In a list, or contains (ignoring case): `:` | `$session.tier : ('gold', 'platinum')`, `$input.text : 'oil'` |
| Logic: `AND` `OR` `NOT`, parentheses | `$session.verified AND NOT ($session.tier = 'basic')` |
| Values | `'text'` or `"text"`, `42`, `3.5`, `true`, `false`, `null`, lists `('a', 'b')` |
| Functions | `exists(x)`, `empty(x)`, `len(x)`, `lower(x)`, `upper(x)` |

A missing value is `null` and compares false to anything but `null`. A string that reads as a number compares as one (`"7" = 7`). In text proto, quote strings with `'…'` to avoid escaping.

**In replies**, `$session.tier` or `${session.tier}` is replaced by its value; `$$` is a dollar sign, and a missing value is empty. A full stop after a reference isn't part of it ("You have $session.points."). Lists read naturally: "Corolla, Civic and Prius".

**Checked at load.** Every expression and reply is parsed when the agent loads; an error says where it is: `flow "main" page "welcome" on_enter step 3 branch if: "$session.tier = gold": column 17: unknown word 'gold' (strings need quotes)`. References must name a declared session param or one of the roots above, `set` keys must be `$session.<name>`, `$page.locals.<name>` or (on a form page) `$form.<slot>`, and defaults must fit their type.

**From the app:** `session->setSessionParam("tier", value)` (a `google::protobuf::Value`) sets one, for example from a CRM when a call starts. Values are kept for the whole conversation, also after it ends and starts over, until the caller disconnects. A wrong type is logged and ignored; a number given for a STRING param, or a numeric string for a NUMBER one, is converted.

## Forms

A form collects several values on one page: it asks for each in turn, re-asks when an answer doesn't fit, reads them back, and waits for a yes. What it collects is a **goal**, written in the agent (`goals { … }`) or in a file of its own:

```textproto
# agent.txtpb
goal_files: ["goals/booking.txtpb"]   # relative to the agent file

pages {
  name: "booking"
  form { goal: "booking" }
  routes { condition: "$form.confirmed" steps { set { key: "$session.day" value: "$form.day" } } target: "booked" }
  routes { condition: "$form.failed" target: "flow:handoff" }
  routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }   # then the form asks again
  routes { intent: "change_day" steps { clear_slot { slot: "day" } } }
}
```
```textproto
# goals/booking.txtpb
# proto-file: vasdk/goal/goal.proto
# proto-message: vasdk.goal.GoalDefinition
name: "booking"
require_confirmation: true
confirm: "So that's $form.service on $form.day, and we'll call you at $form.phone. Shall I book it?"
slots { name: "service" type: ENUM values: ["oil change", "tire repair"] prompt: "Which service?" }
slots { name: "tires" type: INTEGER min: 1 max: 4 condition: "$form.service = 'tire repair'" prompt: "How many tires?" }
slots { name: "day" type: ENUM values: ["monday", "tuesday", "saturday"]
        prompt: "Which day works for you?" reprompt: "Sorry, which day? We're open Monday to Saturday." }
slots { name: "phone" type: PHONE prompt: "What's the best number to reach you?" }
```

```
bot: Which service?
you: a tire repair on Saturday          ← both filled: the service, and a day named in passing
bot: How many tires?                    ← asked only for a tire repair
you: three
bot: What's the best number to reach you?
you: five five five, one two three, four five six seven
bot: So that's tire repair on saturday, and we'll call you at 555-123-4567. Shall I book it?
you: no, make it Tuesday                ← changed straight away, then read back again
bot: So that's tire repair on tuesday, …  Shall I book it?
you: yes                                → $form.confirmed: the first route goes to "booked"
```

**Reading answers.** Each answer is first tried as the slot just asked for:

| Type | Read from the answer | Stored as |
|---|---|---|
| `ENUM` | the longest of `values` found as whole words, ignoring case and punctuation | the value as declared |
| `INTEGER`, `NUMBER` | the first number, in digits or words ("twenty five", "a hundred and two"); `min`/`max` checked | a number |
| `BOOLEAN` | yes or no ("yeah sure", "nope"); the first one said wins | `true`/`false` |
| `PHONE` | the digits, written or spoken one by one ("double four" too); 7 or 10 needed | `555-123-4567` |
| `STRING`, `EMAIL`, `DATE`, `TIME`, `DATETIME` | the whole answer; dates and times only in ISO form for now | the text |

Any other empty `ENUM` slot whose value is named is filled too, so "an oil change on Tuesday" fills two. That also happens with the words that led to the page.

**Order.** The form asks for the first required slot still empty, in the goal's order; a slot with a `condition` is required only while it holds (it reads `$form.<slot>` only). `optional` slots aren't asked. With `require_confirmation`, it then speaks `confirm`, and:
- *yes*: `$form.confirmed` becomes true;
- *no* with a new `ENUM` value ("no, Tuesday"): it changes and is read back again;
- a plain *no*: "What would you like to change?" (or the goal's `change` text). Naming a value changes it; naming a slot ("the phone number") empties it and asks for it again.

**When an answer fits nothing,** the page's routes get it, so "what are your hours" or "talk to a person" still work. A route that stays on the page is followed by the form's next question. With no route, the slot's `reprompt` is asked (default: "Sorry, I didn't get that." and the prompt), and `matched` is false. After more than `max_reprompts` (default 2) answers in a row that don't fit, `$form.failed` becomes true.

**Routes.** Once the form has everything (and a yes, with confirmation) or has failed, the first condition route that holds is taken. `$form.<slot>`, `$form.complete`, `$form.confirmed` and `$form.failed` can be read on the page. The values start empty each time the conversation arrives on the page, and are kept on `CURRENT_PAGE`. To keep them, copy them into session params on the way out.

A slot can also be read as an [entity](#entities): `slots { name: "service" entity: "@service" }` accepts its synonyms, stores its value, speaks its spoken form, and gives `$form.service.price`.

| Step | Example | What it does |
|---|---|---|
| `clear_slot` | `clear_slot { slot: "day" }` | Empties a slot: the form asks for it again, and confirms again. |
| `set` | `set { key: "$form.phone" value: "$session.phone" }` | Fills a slot, checked like an answer: prefill from what the app knows, in `on_enter`. |

**Checked at load:** an unknown goal, `$form` on a page without a form, an `ENUM` with no values, a slot condition reading anything but `$form.<slot>`, `clear_slot` of an unknown slot, `require_confirmation` without `confirm` text. A form page without a condition route gets a warning.

## Entities

An entity is a kind of value the assistant recognises in what people say, declared once and used by name (`@service`) in routes, forms, conditions and replies.

```textproto
entity_types {
  name: "service"
  list {
    values { value: "oil_change"  spoken: "an oil change" synonyms: ["oil change", "lube", "change the oil"]
             attributes { fields { key: "price" value { number_value: 49 } } } }
    values { value: "tire_repair" spoken: "a tire repair" synonyms: ["tire", "tyres", "flat", "puncture"]
             attributes { fields { key: "price" value { number_value: 89 } } } }
  }
}
entity_types { name: "plate"      pattern { regex: "[a-z]{2}[0-9]{3}[a-z]{2}" spelled: true uppercase: true } }
entity_types { name: "party_size" derived { base: "@sys.integer" min: 1 max: 12 } }
entity_types { name: "free_time"  dynamic: true list { values { value: "09:00" spoken: "9 AM" synonyms: ["nine", "morning"] } } }
```

| Kind | Matches | Its value |
|---|---|---|
| `list` | the `value`, `spoken` form or any synonym, as whole words, ignoring case and punctuation | `value`; replies say `spoken` |
| `pattern` | the regex (ECMAScript, ignoring case) against a run of words joined by spaces, or by nothing with `strip_spaces`. With `spelled`, words are read letter by letter first: "alpha bravo one two" → `ab12`; letters and digits as the STT writes them ("AB 12") work too | the words joined; `uppercase` stores them in upper case |
| `derived` | what its `base` matches (`@sys.*`, a list or a pattern), within `min`/`max` and where `condition` (on `$value`) holds | the base's value |

**System entities**, with nothing to declare: `@sys.integer` and `@sys.number` (digits or words: "twenty five", "a hundred and two", "2.5"; "five five" is two numbers), `@sys.phone` (7 to 11 digits in a row, written or spoken, "double four" too) and `@sys.yes_no`.

**Where they're used:**

| Where | Example |
|---|---|
| Routes | `routes { entity: "@service" condition: "$entity = 'tire_repair'" target: "tires" }`. With an intent too, both must match; with no intent, entity routes are tried after the intent routes and before the condition-only ones. |
| `$entity` | In that route's condition and steps: `"$entity takes $entity.minutes minutes."` |
| Form slots | `slots { name: "service" entity: "@service" }`: read as the entity, stored as its value. List and pattern entities named in passing fill their slots too. |
| `$nlu.entities.<entity>` | Found in this turn's words, on any page: `exists($nlu.entities.plate)`. |
| `extract` | `extract { entity: "@plate" into: "$session.plate" }` stores this turn's match, if any. |
| `$entities.<entity>` | Every value of a list entity, spoken: "We have $entities.free_time free." → "We have 9 AM, 11 AM and 2 PM free." |

Each reference reads the value in conditions (`$form.service = 'oil_change'`) and the spoken form in replies ("You're booked for $form.service" → "an oil change"). Fields give the rest: `.value`, `.spoken`, `.raw` (the words heard), `.entity`, and any attribute (`$form.service.price`).

**Dynamic entities** have values that change per conversation: the times still free today, the caller's cars. The declared values are defaults; the app replaces them with `session->setEntityValues("free_time", values)` (a list of `vasdk::nlu::EntityValue`), kept until the caller disconnects. A test case sets them with `entities { name: "free_time" values { … } }`.

**When entities overlap** in one sentence ("a flat tire" holds `@service` and `@part`'s "tire"), the longest match wins, then pattern over list over derived, then the first declared. A single entity asked for by name (a route, a slot, `extract`) is matched on its own.

**Checked at load:** a synonym of two values of one entity, a regex that doesn't compile, a derived base that isn't a system, list or pattern entity, a derived condition reading anything but `$value`, `dynamic` on a non-list, and any unknown `@name`.

## Events

With an agent, listeners also get `page_changed` (`from_flow`/`from_page` → `to_flow`/`to_page`, with the reason `start`, `restart`, `route:<intent>` or `goto`) and `session_ended`. See [VAEngine events](engine.md#events).

## Checking and testing: `vasdk_agentc`

```
$ vasdk_agentc lint agents/garage-menu/agent.txtpb
agents/garage-menu/agent.txtpb: ok (3 flows, 5 pages, 10 intents, 2 entities)

$ vasdk_agentc test agents/garage-menu/agent.txtpb agents/garage-menu/tests/*.txtpb
PASS book a tire repair, then say goodbye
PASS the booking form: values on the way in, a closed day, the hours, and changing the answers
PASS asking for a person hands off and ends
…
10 of 10 passed

$ vasdk_agentc chat agents/garage-menu/agent.txtpb
bot: Hi, this is Acme Garage. Would you like to book a service, hear our hours, or talk to a person?   [main/welcome]
you: I'd like to bring my car in
bot: Sure. Is it for an oil change or a tire repair?   [main/booking]
```

- **`lint`** prints every problem with where it is. Errors stop the agent from loading (`flow "main" page "welcome" route 1: unknown intent "bok"`): a missing start flow or page, an unknown intent or target, a duplicate name, an empty step. Warnings don't: a page never reached, an intent no route uses, a page with no routes.
- **`test`** plays [`FlowTestCase`](../../VASDK/proto/vasdk/flow/test_case.proto) files by text and reports each difference. A turn with no `user` is the conversation start:

```textproto
# proto-file: vasdk/flow/test_case.proto
# proto-message: vasdk.flow.FlowTestCase
name: "book a tire service"
turns { expect { page: "welcome" said_contains: "Acme Garage" expects_reply: true } }
turns { user: "I have a flat" expect { page: "service" } }
turns { user: "my windshield is cracked" expect { matched: false said_contains: "can't help with that" } }
```

  Expectations: `page`, `flow`, `said` (the whole reply), `choices` (the labels offered, in order), `said_contains` (case-insensitive), `matched`, `expects_reply`, `ends_session`, `session { key: "service" value: "an oil change" }` (a param as a reply would say it), `form { key: "day" value: "tuesday" }` (a slot, or `complete`/`confirmed`/`failed`; `""` when empty), `entities { key: "service" value: "oil_change" }` (`$nlu.entities`), and `event` (`no-match`, `no-input`, `max-reprompts`). A turn with `no_input: true` is silence instead of words. A case can set session params and context before the first turn: `session { fields { key: "tier" value { string_value: "gold" } } }`, `context { … }`. The same runs from C++ with `va::flow::runFlowTest`, and the example's cases run in `ctest` (`vasdk_agent_garage_menu`).
- **`chat`** talks to the agent by typing, showing the page (and any event) after each reply, the choices offered, and the parts that can't be interrupted. An empty line is silence (no-input). `--session tier=gold` sets a session param first.
- **`try`** shows what a sentence matches, without running the conversation:

```
$ vasdk_agentc try agents/garage-menu/agent.txtpb "can I book a lube for the afternoon"
intent: book (1 word)
@service = oil_change  spoken "an oil change"  heard "lube"  price=49  minutes=30
@available_time = 14:00  spoken "2 PM"  heard "afternoon"
$ vasdk_agentc try agents/garage-menu/agent.txtpb --entity @sys.phone "five five five, one two three, double four six seven"
@sys.phone = 5551234467  heard "five five five one two three double four six seven"
```

## In C++

| Type | Use |
|---|---|
| `flow::CompiledAgent` | `load(path)` or `compile(proto)`: checked once, immutable, shared by every session that runs it. |
| `flow::FlowRuntime` | One conversation's place in an agent: `start()`, `handleText(text)` and `handleNoInput()` → `Reply` (text, its segments, choices, ui, matched, intent, event, expects reply, ends session, page changes), `replyInterrupted()`, `noInputMs()`, `setSessionParam`, `setEntityValues`, `setContextSource`. Copyable, way back included. |
| `flow::Expression` | `parse(text)` once, then `evaluate(lookup)` or `test(lookup)`; `renderTemplate` for replies. |
| `flow::DialogProcessor` | The pipeline stage `Session` adds for `Options::agent`, after the turn assembler. |
| `flow::runFlowTest` | Plays a `FlowTestCase` and returns what differed. |
| `flow::extractSlotValue`, `extractYesNo` | How a form reads a slot's value from an answer. |
| `flow::EntityCatalog` | `agent->entities()`: `find(name, text)`, `findAll(text)`, `describe(name, value)` → `EntityMatch` (value, spoken, raw, attributes). |
| `goal::GoalState` | A form's values: `updateSlot`, `clearSlot`, `missingSlots`, `isConfirmed`. Shared with the LLM goal path (`GoalManager`). |

## Not yet

These come in later milestones: arithmetic and dates in expressions (`days_until`, `now`), filters in replies (`|raw`), spoken dates and times in forms ("next Tuesday"), skipping a slot ("I don't know"), fuzzy and sound-alike matching of entity values, `@entity` inside intent examples, entities built from others ("a white Toyota from 2019"), values filled by tools, guards and before/after steps in envelopes, numbers like "the second one" for choices, a different voice or speed per part, and custom steps written in C++.
