# Agents

*Reference for agent files and `va::flow`. Schema: [`vasdk/flow/`](../../VASDK/proto/vasdk/flow/) (start with [`agent.proto`](../../VASDK/proto/vasdk/flow/agent.proto)); headers in [`VASDK/include/VASDK/flow/`](../../VASDK/include/VASDK/flow/) - the source of truth if this page and the code ever disagree.*

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
| `intents` | What the user wants, by example: an intent matches when the user's words contain one of its `examples`, in order, as whole words ("book" doesn't match "bookkeeping"). Case and punctuation are ignored. See [Understanding](#understanding) for misspellings, entities in examples, negation and the built-in `sys.*` intents. |
| `flows` | Named groups of pages. `target: "flow:handoff"` goes to another flow's start page, remembering where it came from: `END_FLOW` goes back (see [Shared behaviour and navigation](#shared-behaviour-and-navigation)). |
| `pages` | `on_enter` steps run each time the conversation enters the page; `routes` are tried against each user turn while it's there. |
| `routes` | `intent` to match, `steps` to run, then `target`: a page of this flow, `flow:<name>`, `END_FLOW`, `CURRENT_PAGE` (enter this page again) or `END_SESSION`. No target: stay on the page without entering it again, and ask its question again unless the steps asked something. |

Steps:

| Step | Example | What it does |
|---|---|---|
| `say` | `say { text: ["Got it.", "Okay."] }` | Speaks one text; with several, a different one each time the step runs. Also `ssml`, `interruptible` and `ui`: see [Rich replies](#rich-replies). |
| `ask` | `ask { say { text: "Which day?" } }` | Speaks a question: the reply expects an answer (`agent_reply.expects_reply`), and the question is repeated when the answer isn't understood. `choices` offer answers as buttons; `expect` says what the answer is: see [Expected input](#expected-input). |
| `goto` | `goto { target: "confirm" }` | Moves to the target at once; the steps after it don't run. |
| `end` | `end {}` | Ends the conversation once the reply has played. |
| `call_tool` | `call_tool { name: "create_booking" args { key: "day" value: "$form.day" } into: "$session.booking" filler: "One moment." }` | Calls a tool and stores its result: see [Tools and security](#tools-and-security). |
| `custom` | `custom { [type.googleapis.com/acme.Points] { into: "$session.points" } }` | A step written in C++, chosen by its params' type: see [Extensions](#extensions-page-types-and-c). |

Everything said in one turn is joined into one reply: a route's steps, then the `on_enter` steps of every page entered.

## Understanding

Intents are understood from their examples, with no model and no LLM. When several match, the longest example found wins.

| What | Example |
|---|---|
| Speech-recognition misspellings | "cancel my apointment" matches `cancel my appointment`. Words of 4 letters or more match when at least 80% alike (an edit, or two letters swapped, in a 5-letter word); shorter ones must be exact. List entity synonyms match the same way ("tyres" is `tires`). An entity can opt out with `matching { fuzzy: false }`, or ask for more or less with `min_similarity`. With an [entity model](#entity-models) an entity's misspellings count only inside the words it marks as that entity, where 60% alike does unless the entity sets its own `min_similarity` (names: "jimmy" is 60% "mommy"), and with a sentence encoder the rules match intents exactly (the encoder understands the rest). |
| Entities in examples | `examples: ["book @service"]` matches "book my tyres": the words around the entity in order, the entity anywhere in the words. Each entity counts as one word. |
| Negation | "I don't want to cancel" doesn't match `cancel`: a negating word (not, no, never, don't, can't, ...) up to three words before the example. `negative_examples: ["don't cancel"]` rules an intent out whenever they're in the words. |
| Built-in intents | `sys.yes`, `sys.no`, `sys.cancel`, `sys.repeat`, `sys.help` and `sys.skip` work in routes without declaring them, with the examples of each of the agent's languages. Declaring an intent of the same name replaces its examples. |
| Did you mean...? | When two routes' intents fit equally well, a `clarify` handler runs. `$nlu.alternatives` lists those intents: `handlers { event: "clarify" steps { say { text: "Did you mean $nlu.alternatives?" } } }`. Without a handler, the first route wins. |

`$nlu.intent` is the matched route's intent. `$nlu.confidence` (0 to 1) is how fully its example matched, and `$nlu.stage` is what understood it (`rules`). `vasdk_agentc try` shows the best three.

**A sentence encoder.** With `providers { sentence_encoder { id: "minilm" } }` in the engine config, a session understands paraphrases its examples miss: "is the shop open on saturday" for `hours`. That is all-MiniLM-L6-v2 (int8), on device; its model is downloaded at build time into `models/minilm`. It runs after the rules, which keep every turn they understand. A paraphrase set checks it (`vasdk_minilm_nlu` in ctest).

**Knowledge.** `knowledge { questions: ["do you do bodywork"] answer: "We don't do bodywork." }` answers questions on any page when no route understands the words, then asks the page's question again.
- Without an encoder, a question matches when nearly all its words that matter are said, misspellings allowed.
- With one, the closest question by meaning matches. It also wins over an intent the model alone matched, when it's closer.

**More stages.** `FlowRuntime::setNlu(std::make_shared<nlu::CascadeNluProvider>(stages, 0.55))` tries stages in order: the rules, then, for instance, an on-device model or an LLM. A later stage is asked only when the earlier ones aren't sure enough. `nlu::NluProvider` is the interface a stage implements.


### Entity models

Found by their words alone, a value one letter off counts anywhere: "say bananas three times" has a tire repair in it ("times", "tires"). With `providers { entity_model { id: "..." } }` in the engine config, a model finds where the agent's list entities are said, and only there do misspellings and sound-alikes count; inside its words a 60% likeness picks the value ("tyers" for `tyres`). Exact values and synonyms still count anywhere. `matching { label: "car repair" }` is what a model is told an entity is (default: its name, "available time" for `available_time`). Three to choose from:

| `id` | What finds the entities | For | Against |
|---|---|---|---|
| `gliner` | [GLiNER](https://github.com/urchade/GLiNER), a zero-shot model on device, told each entity's `label`. Build with `-DVASDK_BUILD_GLINER_NER=ON` (183 MB, English; `-DVASDK_GLINER_MODEL=multi` for the multilingual one, 349 MB). `GlinerOptions { threshold }`, default 0.5. | Offline, no training, changing values (`setEntityValues`) need nothing, a multilingual model | Only as good as the labels, and short domain words are hard for it: on the garage agent it finds "tire repair" (0.66) but not "oil change" or "tyres". Loading takes about half a second |
| `tagger` | A tagger trained when a session starts, from the agent's examples with placeholders (`"book @service"`, each filled with the entity's values), each value alone, and the examples without one as words that aren't. It reads MiniLM's word vectors, so it needs `sentence_encoder { id: "minilm" }`. | No new model; fits the agent; trains in under 0.1 s on the garage agent | English (MiniLM); only as good as the examples with placeholders; tags words around a value too ("my tyres are flat"), which is harmless as the value must still be in them |
| `llm` | The LLM the agent's understanding asks (`llm`, or `understanding.llm_profile`): it copies out the words that say each entity. | Messy speech and misspellings ("tyers", "chaning"), any language | Network, and up to `llm.intent_timeout_ms` a turn; never sees a sensitive answer |

In the engine config:

```textproto
providers {
  sentence_encoder { id: "minilm" }   # needed by tagger; makes the rules match intents exactly
  entity_model {
    id: "gliner"                      # or "tagger", or "llm"
    options { [type.googleapis.com/vasdk.providers.GlinerOptions] {
      threshold: 0.4                  # a span's score must reach it, 0 to 1; default 0.5
      model_dir: "models/gliner"      # default: <models_dir>/gliner
    } }
  }
}
```

And in the agent, per entity type:

```textproto
entity_types {
  name: "service"
  list { ... }
  matching { label: "car repair" min_similarity: 0.85 }   # also fuzzy: false, phonetic: true
}
```

To tune, compare with `eval` (below), and look at one sentence with `try`, which prints each span the model found (`tagger: @service "my tyers are flat" (0.84)`):

- `gliner` depends on the words of `label`: try a few ("car repair", "vehicle service"). On the garage agent a lower `threshold` (0.3, 0.2) found no more.
- `tagger` learns only from the agent: more intent examples with placeholders (`"book @service for @sys.date"`), more values and synonyms.
- `llm` is as good as the LLM it asks; a fast one through `understanding.llm_profile`.

A model of your own is an `nlu::EntityExtractor` registered under an id ([Provider interfaces](providers.md#entityextractor)).

On the garage agent's sets (`garage_agentc eval <agent> <set> --engine-config <file>`), the service F1 is 1.00 on the written set whatever the model, and on the recorded-speech set 0.94 without a model, with `gliner` or with `tagger`, and 1.00 with `llm`, the only one that reads "TIRPEC" as a tire repair. With the sentence encoder the rules are exact: the recorded set's intents go from 95.7% to 98.6%, and the written set loses "reprasentative" (95.7%).

## With an LLM

With an `llm` endpoint in the engine config, a session asks the LLM what the agent can't answer by itself. Turns the rules are sure of never reach it, and the same agent runs offline without it.

| Where | What the LLM does | Without an LLM |
|---|---|---|
| Understanding | The last stage, after the rules and MiniLM: it's shown the agent's intents with their examples and picks one (`$nlu.stage` is `llm`). An intent the agent doesn't declare counts as none. | Not understood: `no-match` |
| Forms | An answer the slot can't read ("the black slippery stuff needs replacing"): it gives the user's words for each slot, and only what the slot's entity or type reads in them is kept. "teleportation" is no `@service`, so it's asked again. | The slot's reprompt |
| `llm` step | `llm { instructions: "Greet the caller by the time of day." allowed_targets: ["booking"] offline { say { text: "Hello." } } }` says the LLM's reply, and moves only to an allowed target. | Its `offline` steps; none: "Sorry, I can't help with that here." |
| Generative page | `type: "generative"` with `GenerativeParams { instructions allowed_targets }`. The page's routes, form and knowledge come first; the LLM answers the rest. | The page's `no-match` handler: its offline answer |

The LLM reads the instructions, the form's slots and their values, and the last 12 turns. Sensitive values are masked in what it reads, and the answer to a sensitive slot never reaches it, not even for understanding. It replies with a JSON object (`say`, `goto`, `slots`); a reply that isn't one is said as it is. A request that fails or takes over 8 seconds counts as no LLM for that turn.

**While it thinks.** The intent check waits at most `llm.intent_timeout_ms` (1.5 s by default), after which the rules' guess stands. An `llm` step or a generative page waits longer, but the user isn't left in silence or stuck behind it:

- After a second without an answer, what the turn has said so far is spoken, followed by one of the step's or page's `filler` phrases, in turn: `llm { instructions: "..." filler: ["Let me think.", "One moment."] }`.
- A turn the user resumes, a restart, a disconnect or `stop()` gives up waiting. The given-up turn's reply (`Reply::givenUp`) is never spoken, and the resumed turn is answered.

**A keyword doesn't take the chat.** A rule that matches only a small part of what was said is not trusted: in "tell me a long story about a book you read in Japan", the word "book" doesn't send the user to the booking page.

- The cascade weighs each rule match by `IntentMatch::explains`, the share of the words the match accounts for. A match that covers a sixth of the words or more keeps its full weight.
- If the LLM answers "none", that beats a weak guess from the rules, and the turn goes to the LLM conversation.
- Offline, the rules' guess still stands.

Once the LLM has answered a turn, it keeps the conversation until a route the user clearly asked for takes it back:

- Only the rules are asked, and an intent counts only when it explains at least a quarter of the words (`understanding.chat_min_share`, 0.25). "I am hard of hearing, help me out." (1 word in 8) stays with the LLM, but "help" on its own, or "when do you close", goes to the agent's route.
- An entity-only route (`entity: "@service"`) needs its entity to be the same share of the words: "a tire repair please" books, a story that mentions a tire repair doesn't.
- An answer from an envelope route doesn't ask the page's question again ("We're open 8 to 6." with no "How can I help?").
- A reply with no intent ("You're welcome.") leaves the conversation with the LLM.

On a generative page the same rules apply to every turn, since the page's LLM has the conversation: in a quiz, "Paris" is an answer, not a guess by the intent check that the user wants directions. A page route still takes the words it explains ("what happens next" on a story page, before a music command's "next").

**Handing words to the LLM.** A route with `target: "LLM"` says the words are the LLM conversation's, though the agent understood them: `routes { intent: "story" steps { say { text: "Stories need the internet." } } target: "LLM" }`. With an LLM the turn goes on to it, as if not understood, and the route's steps aren't said; without one, they're the answer. The conversation stays on the page either way, and `Reply::toLlm` is set (test cases check it with `to_llm`). It's how an agent keeps a topic from its commands without answering it itself: "let's play trivia" isn't the music's "play" ([VASDK_Car's companion](../../VASDK_CAR_APP/verticals/companion/companion.txtpb)). Keep the steps to what's said: with an LLM, they run and are undone. `llm_profile: "smart"` on the route has the engine config's profile of that name answer, and the turns after it that nothing understood, until a route answers one ([Different LLMs for different tasks](engine-config.md#different-llms-for-different-tasks)). An `llm` step and a generative page take `profile` too.

`Session::Options::llmCompletion` replaces the config's endpoint (`llm::Completion`: one request, its reply). Turns the agent doesn't understand on a plain page still go to the session's LLM conversation (`LLMProcessor`), as before.

## Rich replies

A reply can carry more than its words:

```textproto
on_enter { say { text: "Welcome to Acme Garage." ui { fields { key: "card" value { string_value: "logo" } } } } }
on_enter { say { text: "Calls are recorded." interruptible: false voice: "cosette" speed: 1.2 } }
on_enter { ask { say { ssml: "<speak>Book a service <break time='300ms'/> or hear our hours?</speak>" }
                 choices { label: "Book a service" value: "book" synonyms: ["make a booking"] }
                 choices { label: "Opening hours" value: "hours" } } }
```

| Field | What it does |
|---|---|
| `interruptible: false` | The user can't talk over this part: it plays to its end. What they say meanwhile is kept and answered once the whole reply has played, like speech over a reply's last words. The parts before and after can still be interrupted. |
| `ssml` | Spoken by a voice provider that reads SSML (`supportsSsml()`). Other voices speak `text`, or the SSML with its tags removed when there's no `text`. References (`$form.day`) work in it. No bundled provider reads SSML yet. |
| `voice`, `speed` | Another of the voice provider's own voices for this part, and how fast (0.8 slower, 1.2 faster; 0.5 to 2): a disclaimer in another voice, a phone number read slowly. The voice is the provider's name for it: Pocket's and MOSS's built-in voices ("cosette", "Ava"), Phonon's voice files, a Cartesia voice id. Speed works on Supertonic and Cartesia (0.6 to 1.5). A provider that can't do one says the part as usual. |
| `ui` | Any structure, given to the app with the reply (`agent_reply.ui_json`): a card, a map, a picture. |
| `choices` (on `ask`) | Answers the app can show as buttons (`agent_reply.choices`, with `choice_values`). A button sends its value as the user's words (`injectUserText`). Saying a label, value or synonym as the whole answer counts as saying the value: "Opening hours" routes like "hours". A choice without a value sends its label. Asked again, the question offers them again. |

**Was the reply heard?** When the user cuts a reply off, the flow is told. A read-back cut off before its end isn't confirmed by a plain "yes" in the next turn: it's read back again, so nobody confirms what they didn't hear. Any other answer (a new day, a "no") counts as usual.

**Checked at load:** a `say` or `ask` with neither text nor SSML, a choice without a label, a speed out of range, and the references in SSML.

## Expected input

Every reply says what the user is expected to say next (`Reply::expectation`, the engine's `agent_reply.expect`), so an app can show a keypad or buttons, and the agent can listen for the right thing. It's derived from the question; an `expect` on an `ask` or a form slot changes it, each field it sets replacing the derived one.

| The reply | Derived expectation |
|---|---|
| asks nothing | `NONE` |
| an `ask` with `choices` / without | `CHOICE` / `OPEN` |
| a form slot | `SLOT`, named in `slot`. `PHONE`: `DIGITS` from 7 to 11 with a `NUMERIC` keyboard; `INTEGER` and `NUMBER`: `NUMERIC`; `EMAIL`: `EMAIL`; `BOOLEAN`: `YES_NO`. A slot of up to 5 values (`ENUM`, or a list entity) offers them as choices. |
| a form's read-back / "what would you like to change?" | `CONFIRMATION` / `OPEN` |

```textproto
on_enter { ask { say { text: "Book or hear our hours?" }
                 choices { label: "Book" value: "book" } choices { label: "Hours" value: "hours" }
                 expect { intents: ["book", "hours"] no_input_ms: 5000 } } }

slots { name: "card" type: STRING prompt: "Please type or say your card number."
        expect { mode: DIGITS min_digits: 16 max_digits: 16 keyboard: NUMERIC secure_entry: true
                 partial_reprompt: "I got $input.digit_count digits. Card numbers have 16." } }
```

| `expect` field | What it does |
|---|---|
| `mode`, `slot`, `keyboard`, `secure_entry` | For the app: what to offer for the answer (a keypad, hidden typing). |
| `intents` | Only these intents are matched on the page while the question is open; "do you sell cars" on a book-or-hours question is not understood. Envelope routes still match. |
| `allow_global: false` | The envelopes' routes (hours, a person) don't match either. |
| `min_digits`, `max_digits` | With `DIGITS`: fewer digits than `min_digits` is a partial answer, reprompted with `partial_reprompt` (default: "I only got 4 digits." and the question); it counts as a miss. On a `STRING` slot the answer is read as its digits, said or written ("it's 4111 1111…" → `4111111…`), and must have between `min_digits` and `max_digits`. |
| `no_input_ms`, `max_reprompts` | The no-input timer and the misses allowed, for this question, before the page's settings (and, on a form, the goal's `max_reprompts`). |
| `follow_up_listen_ms` | How long the mic stays open after the question (see below). |
| `turn_end` | When the answer is over. `PATIENT`: pauses don't end it, so "five five five... one two three... four five six seven" stays one turn (the VAD holds pauses twice as long, at least 1 s, and the end at least 1.5 s; the turn model's guess during a pause is ignored). `SNAPPY`: ends sooner after the user stops (at most 300 ms). `NORMAL`: the engine's turn-taking. Unset: `PATIENT` for `DIGITS` and an `EMAIL` keyboard, `SNAPPY` for `YES_NO` and `CONFIRMATION`. |
| `hints` | Words the answer may hold, for the speech recognizer. Added to those derived: the choices' labels, the asked slot's values and synonyms, and the page's intent examples (up to 50). Recognizers that can be biased use them for the next answer: Whistle favours them as keywords, with its own, and Soniox sends them as its `context` terms. The others don't take hints (Deepgram's client has only Nova-2's `keywords`, not Nova-3's `keyterm`). |

**Tapping a choice.** `Session::selectChoice(index)` (from 0) taps one of the last reply's choices, the same as saying its value; `vasdk_agentc chat` taps with `#1`, and test cases with `choice: 0`.

**How long the mic stays open after a reply.** The innermost value set wins: a `say`'s own `follow_up_listen_ms`, then the question's `expect`, then `settings { follow_up_listen_ms }` in the page's envelopes, the flow's and the agent's, then the engine's (`Session::setFollowUpListenMs`, else the config's [`activation.follow_up`](engine-config.md#activation)). `0` closes the mic at once ("Goodbye!"), `-1` never. After a question with a no-input timer, the mic stays open at least until its reprompt starts. A proactive message's own window comes with triggers (milestone 17).

**Checked at load:** unknown intents in `expect`, `min_digits` above `max_digits`, negative timers, a window below `-1`, and the references in `partial_reprompt` (`$input.digit_count` is the most digits the user said in a row).

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
  settings { no_input_ms: 8000 max_reprompts: 2 follow_up_listen_ms: 10000 }
}
envelope_defs { name: "booking" extends: "standard" settings { max_reprompts: 1 } }
flows { name: "booking" envelopes: ["booking"] … }   # a flow's pages; pages take `envelopes` too
```

**Layering.** For each turn, the page's own routes come first, then the page's envelopes, the flow's and the agent's, in the order listed, each followed by the envelope it `extends`. A page route that matches wins. Otherwise the envelopes' intent routes compete, as a page's do: the best score wins, wherever it is, so "stop navigation" is the navigation envelope's even when a music envelope listed first has "stop". On a tie, the first layer's, so an inner envelope's route for the same intent replaces an outer one's. With no intent match, the first layer's entity or condition route is taken. Handlers and settings are found in layer order: the first layer that has one wins, so an inner one replaces an outer one. An envelope's targets can't name a page (it would be a different page wherever it's used): `flow:<name>`, `END_FLOW`, `END_SESSION` or `CURRENT_PAGE`.

**Guards, before and after.** An envelope can also hold what runs around every turn of its pages:

```textproto
envelope_defs {
  name: "account"
  # What its pages need: entering one, or taking a turn there, without it runs the guard instead.
  guards { condition: "$session.verified" steps { say { text: "Let me check it's you first." } } target: "flow:verify" }
}
envelope_defs {
  name: "standard"
  before { extract { entity: "@license_plate" into: "$session.plate" } }   # a plate said at any point
  before { set { key: "$session.turns" value: "$session.turns + 1" } }
  after  { branch { if: "$session.turns = 20" then { say { text: "We've been at this a while: say 'a person' any time." } } } }
}
```

- **Guards** come first, outermost envelope first. The first whose `condition` doesn't hold runs its steps, then moves to its `target`, instead of the page being entered or the turn being taken. Its target flow is exempt, so `flow:verify` can carry the same envelope, and its `END_FLOW` comes back to the page the user was on before the guard sent them away. With no target, the user stays where they were and is asked the question again.
- **Before** steps run next, after the envelopes written in C++, before the page's routes. What they say comes before the reply; a step that moves (`goto`, `end`) answers the turn.
- **After** steps run once the reply is ready, innermost envelope first, before the C++ envelopes' `after`. They can say, set, extract and call tools, but not move or ask: `goto`, `end`, `ask` and `llm` are refused when the agent loads.

**Handlers** replace the built-in behaviour for an event. Pages take `handlers` too:

| Event | When | Built in |
|---|---|---|
| `no-match` | No route understood the user | "Sorry, I can't help with that here." (the language's `replies`) and the question again |
| `no-input` | Silence for `no_input_ms` after a question has played | The question again |
| `max-reprompts` | More than `max_reprompts` (default 2) no-match or no-input in a row on a page | The no-match or no-input behaviour again |
| `resumed` | The conversation picked up where an earlier one left it (below): its steps run before the page is entered, its `target` isn't used | Nothing: the page is entered |

**Picking up a conversation later.** `FlowRuntime::snapshot()` says where a conversation is, as a [`vasdk.flow.ConversationSnapshot`](../../VASDK/proto/vasdk/flow/snapshot.proto): its flow and page, the form's filled slots and the session params. Sensitive slots and params are left out, so it can be stored. `FlowRuntime::resume(snapshot)` starts there instead of at the start page: the params are set, the page is entered with the form's saved slots already filled, so only the rest are asked, and the page's `resumed` handler speaks first ("Welcome back."). A snapshot whose page the agent no longer has starts over. A `Session` gives the last snapshot with `conversationSnapshotMessage()`, or its JSON form with `conversationSnapshot()`, and `resumeConversation(snapshot)` (either) before `start()` makes its greeting resume. VASDK_CallCenter uses them for callers who hang up mid-form (`--persist-dir`).

The handler's steps run, then it moves to its `target`. If it stays on the page and asked nothing, the page's question is asked again (on a form page, the form's). The no-input timer runs only when a `no-input` handler or `no_input_ms` applies to the page; it starts once the question has finished playing and stops when the user starts speaking. On a form page, answers that don't fit are re-asked by the form, and `max-reprompts` runs once the form fails and no `$form.failed` route takes it. `Reply::event` says which one ran; test cases check it and `vasdk_agentc chat` shows it.

**Flows as digressions.** `flow:<name>` remembers the page it left, with its form values and question. `END_FLOW` goes back there and asks that question again:

```
bot: Which day works for you?                [main/booking]
you: how much is an oil change               → flow:prices, which says the price, then END_FLOW
bot: An oil change costs 49 dollars. Which day works for you?   [main/booking]
```

Flows nest up to 8 deep; deeper, the way back isn't kept. `END_FLOW` in the start flow ends the conversation, and `END_SESSION` forgets every way back.

## Extensions: page types and C++

**Page types.** A page with `type` gets the type's steps, routes and handlers along with its own: its `on_enter` runs first, its routes and handlers come right after the page's. The page's `params` are typed by the type's `params_type`, and read as `$params.<field>`.

```textproto
proto_imports: ["acme.proto"]    # message InfoParams { string text = 1; string more = 2; }
page_types {
  name: "info"
  params_type: "acme.InfoParams"
  on_enter { ask { say { text: "$params.text Want more?" } } }
  routes { intent: "more" steps { say { text: "$params.more" } } target: "done" }
}
pages { name: "hours"  type: "info" params { [type.googleapis.com/acme.InfoParams] { text: "Open 8 to 6." more: "Saturdays too." } } }
pages { name: "prices" type: "info" params { [type.googleapis.com/acme.InfoParams] { text: "Oil is 49." more: "Tires are 89." } } }
```

Built in (params in `vasdk/flow/page_types.proto`); one declared in `page_types` with the same name replaces it:

| Type | Params | What it does |
|---|---|---|
| `menu` | `MenuParams { prompt options { label value synonyms target } }` | Asks the prompt with the options as choices. Naming one (its label, value or a synonym, as the whole answer or within it: "um, how much is it") or tapping it moves to its `target`; anything else goes to the page's routes. |
| `yes_no` | `YesNoParams { question yes_target no_target }` | Asks, expecting yes or no, and moves to the target for the answer; anything else goes to the page's routes. |
| `end` | `EndParams { text }` | Says the text and ends the conversation. |

A form is still the page's `form { goal }`, not a page type.

**Params types.** `proto_imports` lists `.proto` files next to the agent: their messages type params with no code generation, in text, JSON and binary agents alike. Types compiled into the app are found without them. A misspelled field fails when the file loads, with its line and column.

**Written in C++.** A `FlowRegistry` holds steps, page types and envelopes, given to `CompiledAgent::load`:

```cpp
auto registry = std::make_shared<va::flow::FlowRegistry>();   // menu, yes_no and end included
registry->addStep<garage::BookingReference>([](const garage::BookingReference& params, va::flow::TurnContext& turn) {
    turn.set(params.into(), va::flow::stringValue(makeReference(turn.lookup("$form.phone"))));
});
va::flow::FlowRegistry::Envelope thanks;
thanks.before = [](va::flow::TurnContext& turn) {   // true: the turn is answered
    if (va::flow::splitWords(turn.text()) != std::vector<std::string>{"thanks"}) return false;
    turn.say("You're welcome.");
    return true;
};
registry->addEnvelope("thanks", thanks);
auto agent = va::flow::CompiledAgent::load("agent.txtpb", &error, nullptr, registry);
```

| Kind | Used as | Runs |
|---|---|---|
| step | `custom { [type.googleapis.com/<params type>] { ... } }` | Where it stands among the steps. A `goTo` moves on once it returns; the steps after it don't run. |
| page type | `type: "<name>"` with its `params` | `onEnter` before the page's `on_enter`; `onTurn` first in each turn on the page: `true` answers it, `false` leaves it to the routes. `targets` lists the targets its params name, checked at load and counted as reachable. |
| envelope | `envelopes: ["<name>"]`, with the text ones | `before` ahead of the page's routes, outermost envelope first (the agent's, the flow's, the page's): it may change the words the routes see (`setText`, to redact them) or answer the turn (`true`). `after` once the reply is ready, innermost first, also after the greeting and no-input. |

What they get, `TurnContext`: `lookup("$session.tier")`, `set(key, value)` (the keys `set` takes), `text()` and `setText()`, `say(text)`, `render(template)`, `ask(question, choices, expect)`, `goTo(target)`, `replyText()`, `flowName()`, `pageName()`. Typed params come as their C++ class, generated from their `.proto`; params of a type loaded with `proto_imports` are converted to it. One registered by name (`addStep("acme.Points", ...)`) gets a `google::protobuf::Message` to read by reflection. Every conversation shares them: keep a conversation's state in its values (`$session.*`, `$page.locals.*`), never in the C++ object.

Answered by a C++ envelope or page type, and staying on the page with nothing asked, the page's question is asked again, as after a route.

**Checked at load:** an unknown page type, step or envelope (with what to register), params of the wrong type or missing, params of a type neither linked in nor imported, `$params` fields that don't exist, and the targets a page type's params name.

## Tools and security

**Calling a tool.** `call_tool` calls a tool by name with its `args` (each an expression) as a JSON object, and stores the result in `into`. A STRUCT session param keeps a whole result, read field by field: `$session.booking.id`. While the tool runs, its `filler` is spoken, so the caller isn't left in silence. With `filler_after_ms: 800`, the filler is said only when the call takes longer than that: nothing for a quick lookup, "One moment." for a slow one. The conversation waits for the tool on a thread of its own, so the rest of the pipeline keeps running; the tools' `timeout_ms` (below) ends a call that takes too long, as a `tool-error`.

```textproto
session_params { name: "booking" type: STRUCT }
routes {
  condition: "$form.confirmed"
  steps { call_tool { name: "save_booking" args { key: "day" value: "$form.day" } args { key: "phone" value: "$form.phone" }
                      into: "$session.booking" filler: "One moment while I book that." } }
  steps { say { text: "Your reference is $session.booking.reference." } }
  target: "booked"
}
handlers { event: "tool-error" steps { say { text: "Our booking system is down. Can I call you back?" } } target: "callback" }
```

A tool that fails (it's unknown, refused, too slow, its reply is too long, or the service says no) raises `tool-error`. The steps after it and the route's target are skipped. Its handler runs; without one, the reply is "Sorry, I can't do that right now." and the page's question again.

**Where tools come from:**
- **Written in C++:** `registry->addTool(va::llm::Tool("save_booking", "...").onCall([](const nlohmann::json& args) { ...; return result; }))`. It runs on a thread of its own and may block. An exception, or a result of `{"error": "..."}`, fails the call.
- **Provided by the app, in any language** ([C](c-api.md), [Kotlin](../apps/android.md#an-agent-before-the-llm)): declare it with its name only, `tools { name: "place_call" description: "Calls a number" }`, so `lint` and test cases (with `tool_results`) know it. The app registers it at runtime; without one, calling it fails.
- **Over HTTP**, declared in the agent:

```textproto
tools {
  name: "create_booking"
  http { url: "https://api.acme.example/bookings"            # POST, args as the JSON body; method: "GET" sends them as the query
         headers { key: "Authorization" value: "Bearer $secrets.acme_api" } }
}
```

**HTTP tools are limited by the engine config.** These settings live in the engine config, never in the agent (see [Security](engine-config.md#security)):
- The URL must match `security.http_allow`. With none listed, nothing is reached.
- Redirects aren't followed, and certificates are checked.
- A reply longer than `max_response_kb` fails the call.
- `$secrets.<name>` in a header reads `security.secrets`.
- A tool of either kind still running after `timeout_ms` fails.

**No secrets in agent files.** Agent files are shared, committed and sent to tools, so loading fails on any of these:
- a `${...}` (agent files can't read the environment)
- a literal that looks like a key or token (`sk-…`, `AKIA…`, `ghp_…`, `xoxb-…`, `AIza…`, a private key, a long `Bearer` token)
- a literal value in an `Authorization`, `*key*`, `*token*` or `*secret*` header
- `$secrets` anywhere but a tool's headers

**Sensitive values.** Mark a slot or a session param `sensitive: true` (a phone or card number). Then:
- Its value is masked wherever the conversation is reported: `***-***-4567`, keeping the last 4 digits of a long number.
- That covers listener events (replies, tool calls) and the logs: the dialog's, and the voice's (`TTSFrameProcessor`) for the replies it says.
- A question for a sensitive slot expects a private answer: `secure_entry`, so the app can hide what's typed. Its answer is marked private from the speech recognizer on (the turn policy tells `TranscriptionFrameProcessor`), so its transcript is masked in events and in the turn assembler's and the dialog's logs, and it never goes on to an LLM. The bundled speech providers don't log what they heard, except at debug level (Gradium).

**Steps written outside C++** (the [C API](c-api.md), Python, [Kotlin](../apps/android.md#an-agent-before-the-llm)) answer with a [`StepOutcome`](../../VASDK/proto/vasdk/flow/step_outcome.proto): values to `set` and `unset`, texts to `say`, a `goto`. `TurnContext::apply` checks it first, and applies all of it or none:
- Only `$session.<declared param>`, `$page.locals.<name>` and `$form.<slot>` can be written.
- A value that doesn't fit its type is refused.
- So is an unknown target.

**In tests**, `tool_results { key: "create_booking" value: "{\"id\": \"B-42\"}" }` on a turn stands in for that tool (`{"error": ...}` fails it). `tools_called: ["create_booking"]` checks which tools were called. Without a stand-in, C++ tools run and HTTP tools are refused, because a test has no engine config to allow them.

## Triggers

A trigger lets the assistant speak first: when the conversation starts, when the app says so, or after a silence. It fires from its `source`. If its `condition` holds, its `action` runs when its `delivery` allows. `$trigger.payload.<field>` reads what the app sent with it, and `$trigger.name` its name.

```textproto
triggers {
  name: "car_ready"
  source { app_event { name: "car-ready" } }      # session->fireTrigger("car-ready", payload)
  action { say { text: "By the way, your $trigger.payload.car is ready for pickup." } }
  delivery { interrupt: AFTER_FLOW priority: HIGH dedupe_key: "$trigger.payload.car" expires_after_ms: 3600000 }
}
triggers { name: "welcome_back" source { session_start {} } condition: "$session.tier = 'gold'"
           action { say { text: "Welcome back, $session.name." } } }
triggers { name: "nudge" source { inactivity { after_ms: 30000 } } action { say { text: "Still there?" } } }
proactive_policy { quiet_from: "22:00" quiet_until: "07:00" max_per_hour: 4 }
```

**Sources:**

| Source | Fires |
|---|---|
| `session_start` | When the conversation starts. Its action replaces the start page's entry: a `say` comes before the greeting, a `goto` enters another page, a `start_flow` runs a flow and then enters the start page. |
| `app_event` | On `Session::fireTrigger(name, payload)`. `name` is the event's; without one, it's the trigger's own name. |
| `inactivity` | After `after_ms` without a user turn, counted from the end of the last reply, while a conversation goes on. Each fires once per silence, shortest first. |
| `schedule` | At a time: `at: "08:00"` every day; `at: "2026-11-09T08:00"` once, local time; or `every_minutes: 30`. References are filled in when it's checked: `at: "${session.booking_day}T08:00"` reminds the caller the morning of their booking. A time that passed before the first check doesn't fire. |
| `context_change` | When `condition`, an expression on `$context`, starts holding (`edge: RISING`, the default), stops (`FALLING`) or either (`BOTH`): `"$context.battery.level < 15"`. Its first value is taken as it is. |

**Actions:**
- `say`: says it, then asks the conversation's question again, unless the reply it's added to already asks one.
- `goto`: a target, as a goto step.
- `start_flow`: enters the flow; `END_FLOW` comes back to where the conversation was.

**Delivery.** With no conversation going on (before it starts, after it ends), a trigger is said at once, from idle: the follow-up window opens after it. During a conversation, `interrupt` decides when:

| `interrupt` | When |
|---|---|
| `AFTER_TURN` (default) | At once if nothing is playing and the user isn't speaking; else once the reply has played. |
| `AFTER_FLOW` | Once the conversation is between tasks: no form being filled, and no flow it will come back from. A car-ready event waits for the booking to finish. |
| `IMMEDIATE` | At once: a reply playing is cut off, as by the cancel button, and the trigger is said as soon as it stops. |
| `NEVER` | Only when no conversation is going on; otherwise dropped. |

**Limits:**
- Queued triggers go highest `priority` first (`LOW`, `NORMAL`, `HIGH`, `CRITICAL`), one per turn.
- `expires_after_ms` drops one that waited too long.
- `dedupe_key` (an expression) keeps one queued per value.
- `cooldown_ms` drops one fired again too soon after it was said.
- `proactive_policy` suppresses all but `CRITICAL` ones in its quiet hours (local time) and caps them at `max_per_hour`.
- `ask_first { text: "I have an update on your order. Want to hear it?" }` asks, expecting yes or no. Yes runs the action; no gets "Okay." and the question again. Any other answer lets the offer go, and the words are a turn as usual.

**Checking schedules and context.** The dialog checks `schedule` and `context_change` triggers every second, when an agent has them. `FlowRuntime::checkTriggers()` does the same by hand. An app that sleeps between conversations can do two things:
- Keep reminders across restarts: save `triggerState()` (JSON) and give it back to `restoreTriggerState()`. A reminder that came due while the app was down is said once, when it's back.
- Wake up when the next one is due: `nextScheduledTrigger()` (on Android, for `AlarmManager`).

On Android, `VasdkEngine.fireTrigger(name, payloadJson)` fires one, from a notification listener for instance.

Each fired trigger is reported in a `trigger_fired` event, with its decision: `STARTED`, `QUEUED`, `DROPPED`, `EXPIRED` or `SUPPRESSED`, and why. In `vasdk_agentc chat`, `!car-ready {"car": "Golf"}` fires one.

## Stored values and conditions

An agent can remember things and branch on them. Declare the values a conversation keeps:

```textproto
session_params { name: "tier"    type: STRING default { string_value: "standard" } }   # STRING, NUMBER, BOOL, LIST or STRUCT
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
| Arithmetic: `+` `-` `*` `/` `%`, and `-x` | `$session.points + 10 * len($form.services)`, `$session.turns % 3 = 0` |
| In a list, or contains (ignoring case): `:` | `$session.tier : ('gold', 'platinum')`, `$input.text : 'oil'` |
| Logic: `AND` `OR` `NOT`, parentheses | `$session.verified AND NOT ($session.tier = 'basic')` |
| Values | `'text'` or `"text"`, `42`, `3.5`, `true`, `false`, `null`, lists `('a', 'b')` |
| Functions | `exists(x)`, `empty(x)`, `len(x)`, `lower(x)`, `upper(x)`, `number(x)`, `round(x)` or `round(x, digits)`, `floor(x)`, `ceil(x)`, `abs(x)`, `min(a, b, …)`, `max(a, b, …)` (or of one list) |
| Dates | `now()` (`"2026-11-05T14:30:00"`), `today()` (`"2026-11-05"`), `days_until(date)`, `days_since(date)`, `add_days(date, n)`, `weekday(date)` (1 Monday … 7 Sunday) |

Operators bind as usual: `-x`, then `*` `/` `%`, then `+` `-`, then the comparisons, `NOT`, `AND`, `OR`. A missing value is `null` and compares false to anything but `null`; in `+` and `-` it counts as 0. A string that reads as a number compares and adds as one (`"7" = 7`), and `+` joins text when either side isn't a number (`'Hi ' + $session.name`). Dividing by zero gives `null`.

Dates are ISO text, as date slots and `@sys.date` keep them, so they also compare in order: `$form.date > today()`, `days_until($form.date) <= 1` (the booking is today or tomorrow), `weekday($value) <= 5` (a derived entity for weekdays). `days_until` takes a date and time too, and gives `null` for text that isn't a date. They count from the conversation's clock: the local date, or the one a test sets with `FlowRuntime::setClock`.

In text proto, quote strings with `'…'` to avoid escaping.

**In replies**, `$session.tier` or `${session.tier}` is replaced by its value; `$$` is a dollar sign, and a missing value is empty. A full stop after a reference isn't part of it ("You have $session.points."). Lists read naturally: "Corolla, Civic and Prius". A form's values are said as a person would: `$form.date` is "Tuesday, November 10th".

Braces also take an expression, with `$` on its references: `${$session.points * 2}`, `${days_until($form.date)}`. Filters after `|` change the value, in order:

| Filter | | Example → |
|---|---|---|
| `raw` | The value, not as said | `${form.date\|raw}` → "2026-11-10" |
| `count` | A list's items | `${session.cars\|count}` → "2" |
| `plural:"one":"many"` | A number (or a list's count) and its noun, in as many forms as the reply's language has (2 to 6) | `${session.cars\|plural:'car':'cars'}` → "2 cars" |
| `round`, `round:digits` | | `${$session.total / 3\|round:2}` → "16.33" |
| `lower`, `upper`, `capitalize` | | `${session.name\|capitalize}` → "Sam" |
| `default:"text"` | For a missing or empty value | `${session.agent\|default:'our team'}` |

A reference is said as a reply says it, unless a filter needs the value itself (`raw`, `count`, `plural`, `round`); an expression always reads the values.

**Checked at load.** Every expression and reply, with its filters, is parsed when the agent loads; an error says where it is: `flow "main" page "welcome" on_enter step 3 branch if: "$session.tier = gold": column 17: unknown word 'gold' (strings need quotes)`. References must name a declared session param or one of the roots above, `set` keys must be `$session.<name>`, `$page.locals.<name>` or (on a form page) `$form.<slot>`, and defaults must fit their type.

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
| `DATE`, `TIME`, `DATETIME`, `EMAIL` | as `@sys.date`, `@sys.time`, `@sys.datetime` and `@sys.email`: "next Tuesday", "half past three", "john dot doe at gmail dot com" | `2026-11-10`, `15:30`; a reply says "Tuesday, November 10th", "3:30 PM" |
| `STRING` | the whole answer | the text |

Any other empty `ENUM` slot whose value is named is filled too, so "an oil change on Tuesday" fills two. That also happens with the words that led to the page.

**Order.** The form asks for the first required slot still empty, in the goal's order; a slot with a `condition` is required only while it holds (it reads `$form.<slot>` only). `optional` slots aren't asked.

**Edge cases:**
- **Skipping.** A `skippable: true` slot is asked, but "skip", "I don't know" or "not sure" leaves it empty (`$form.<slot>` is `""`) and moves on. Said for any other slot, those words aren't an answer, and the slot is asked again.
- **Lists.** A `list: true` slot (`ENUM`, or an entity) keeps every value named, in the order said: "a wax and a car wash" → `["wax", "wash"]`. A reply says "a wax and a car wash".
- **Corrections.** Changing an answer ("no, make it an oil change") clears the answers whose `condition` reads it, such as the tire size. They're asked again only if they still apply. With `require_confirmation`, it then speaks `confirm`, and:
- *yes*: `$form.confirmed` becomes true;
- *no* with a new `ENUM` value ("no, Tuesday"), or the new value alone ("Tuesday", "to John"): it changes and is read back again;
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
| `list` | the `value`, `spoken` form or any synonym, as whole words, ignoring case and punctuation. Misspellings match too (see [Understanding](#understanding)); `matching { phonetic: true }` also matches words of 3 letters or more that sound alike (Double Metaphone, either of a word's codes), for names ("Catherine" for `katherine`, "Steven" for `stephen`, "Schmidt" for `smith`) | `value`; replies say `spoken` |
| `pattern` | the regex (ECMAScript, ignoring case) against a run of words joined by spaces, or by nothing with `strip_spaces`. With `spelled`, words are read letter by letter first: "alpha bravo one two" → `ab12`; letters and digits as the STT writes them ("AB 12") work too | the words joined; `uppercase` stores them in upper case |
| `derived` | what its `base` matches (`@sys.*`, a list or a pattern), within `min`/`max` and where `condition` (on `$value`) holds | the base's value |
| `composite` | one of its `templates` in order: `"@color:color @make:make from @sys.integer:year"`. Each part is a match of that entity starting right there; the words between them are said as they are. The longest template found wins, and a composite beats its parts | a struct, a field per part: `{color: "white", make: "toyota", year: 2019}`; replies say the parts as said ("white Toyota from 2019"). A form slot can take one (`entity: "@vehicle"`): `$form.vehicle.value.make`, and `$form.vehicle` said as the caller said it |
| `code` | what the app's code finds: `code { extractor: "plates_db" }`, registered with `FlowRegistry::addEntityExtractor("plates_db", ...)`. It gets the words, returns where each value was said (`start`, `length`), and its `value`, `spoken` form and `attributes`: a database lookup, the phone's contacts. An extractor that isn't registered fails the agent's load | its `value`; replies say `spoken` |

**An agent in several files.** `include_files: ["climate/climate.txtpb", "media/media.txtpb"]` merges in more of the agent, relative to this file. Each file is a `vasdk.flow.Agent` with its own intents, entities, envelopes, flows, triggers, knowledge and goals, so a feature's part of the conversation can live next to its code. Files are merged in order, after the main file's own content. Each file's `goal_files`, `entity_files`, `language_files` and `include_files` are relative to that file. Only the main file sets `name`, `locale`, `languages`, `start_flow`, `schema_version`, `proactive_policy` and `proto_imports`. A file that includes itself, a missing file, and the same names declared twice fail the load. Parts' envelopes compete on how well they match, and their order breaks ties. See [VASDK_Car](../apps/car.md), built from ten such parts.

**Entity packs.** `entity_files: ["../shared/cars.txtpb"]` imports entity types from files of their own (`vasdk.nlu.EntityPack`), relative to the agent, so agents can share them. A type the agent declares itself wins over a pack's. VASDK ships three in [`VASDK/entities`](../../VASDK/entities/): `@country` (197 countries, with `iso2` and `iso3` attributes), `@us_state` (the states and DC, with their postal `code`) and `@currency` (32, with their ISO `code`). Their values are snake_case names (`united_kingdom`, `new_york`, `us_dollar`), never codes, because many codes are also words ("in", "or", "try").

**System entities.** These need no declaring. Each reads the agent's languages as speech recognizers write them, in words or in digits; the examples below are VASDK's built-in English ([Languages](languages.md)). Dates count from the conversation's clock.

| Entity | Example → value | Said in a reply |
|---|---|---|
| `@sys.integer`, `@sys.number` | "twenty three point five", "23.5" → `23.5`; "five five" is two numbers | the number |
| `@sys.phone` | "five five five, one two three, double four, six seven", "555 123 4467" → `5551234467` (7 to 11 digits) | `555-123-4467` |
| `@sys.digits` | any number of digits in a row, the same way | the digits |
| `@sys.yes_no` | "yeah sure" → `true`, "nope" → `false` | |
| `@sys.date` | "next Tuesday" (the Tuesday of next week), "Tuesday", "the fifth", "November 20th", "the 3rd of January", "in two weeks", "tomorrow", "2026-12-24" → `2026-11-10` | "Tuesday, November 10th" |
| `@sys.time` | "half past three in the afternoon", "3:30 pm", "quarter to five", "nine oh five", "at noon", "at 9" → `15:30` | "3:30 PM", "noon" |
| `@sys.datetime` | "tomorrow at noon" → `2026-11-03T12:00` | "Tuesday, November 3rd at noon" |
| `@sys.duration` | "an hour and a half", "45 minutes", "half an hour", "two days" → `PT1H30M` | "1 hour and 30 minutes" |
| `@sys.email` | "john dot doe at gmail dot com", "John.Doe@Gmail.com" → `john.doe@gmail.com` | the address |
| `@sys.ordinal` | "the second one", "2nd", "number two" → `2` | the number |

A bare number is neither a time nor a date ("I have 3 cars"). With choices on offer, "the second one" or "number two" picks the choice in that place.

**Speaking them.** A voice that doesn't read numbers well itself is given text it can say, in the reply's language. That is the default: `GenericVoiceSynthesisProvider::normalizesText()` is false.
- ISO dates are said as dates.
- 24-hour times are said as the language says times (with AM or PM in English).
- Phone numbers are said digit by digit, in their groups: "five five five, one two three, four five six seven".

**Where they're used:**

| Where | Example |
|---|---|
| Routes | `routes { entity: "@service" condition: "$entity = 'tire_repair'" target: "tires" }`. With an intent too, both must match; with no intent, entity routes are tried after the intent routes and before the condition-only ones. |
| `$entity` | In that route's condition and steps: `"$entity takes $entity.minutes minutes."` |
| Form slots | `slots { name: "service" entity: "@service" }`: read as the entity, stored as its value. List, pattern and code entities named in passing fill their slots too ("text Sam" fills the contact). |
| `$nlu.entities.<entity>` | Found in this turn's words, on any page: `exists($nlu.entities.plate)`. |
| `extract` | `extract { entity: "@plate" into: "$session.plate" }` stores this turn's match, if any. |
| `$entities.<entity>` | Every value of a list entity, spoken: "We have $entities.free_time free." → "We have 9 AM, 11 AM and 2 PM free." |

Each reference reads the value in conditions (`$form.service = 'oil_change'`) and the spoken form in replies ("You're booked for $form.service" → "an oil change"). Fields give the rest: `.value`, `.spoken`, `.raw` (the words heard), `.entity`, and any attribute (`$form.service.price`).

**Dynamic entities** have values that change per conversation: the times still free today, the caller's cars. The declared values are defaults; the app replaces them with `session->setEntityValues("free_time", values)` (a list of `vasdk::nlu::EntityValue`), kept until the caller disconnects. A test case sets them with `entities { name: "free_time" values { … } }`.

**When entities overlap** in one sentence ("a flat tire" holds `@service` and `@part`'s "tire"), the longest match wins, then pattern over list over derived, then the first declared. A single entity asked for by name (a route, a slot, `extract`) is matched on its own.

**Checked at load:** a synonym of two values of one entity, a regex that doesn't compile, a derived base that isn't a system, list or pattern entity, a derived condition reading anything but `$value`, `dynamic` on a non-list, and any unknown `@name`.

## Events

With an agent, listeners also get these events:
- `page_changed`: `from_flow`/`from_page` → `to_flow`/`to_page`, with the reason `start`, `restart`, `route:<intent>` or `goto`.
- `tool_call`: a `call_tool`'s name, its arguments and result, with sensitive values masked.
- `session_ended`. See [VAEngine events](engine.md#events).

## Checking and testing: `vasdk_agentc`

An agent that uses steps, page types or envelopes written in C++ needs a copy of the tool that registers them: `flow::runAgentTool(argc, argv, registry)` is the whole of it ([garage_agentc.cpp](../../VASDK_APP/agents/garage-menu/extensions/garage_agentc.cpp)). `vasdk_agentc` itself names what's missing. The example agent uses `garage_agentc`.

```
$ garage_agentc lint agents/garage-menu/agent.txtpb
agents/garage-menu/agent.txtpb: ok (3 flows, 5 pages, 10 intents, 2 entities)

$ garage_agentc test agents/garage-menu/agent.txtpb agents/garage-menu/tests/*.txtpb
PASS the app replaces the free times for the day
PASS book a tire repair, then say goodbye
PASS the booking form: values on the way in, a closed day, the hours, and changing the answers
…
13 of 13 passed

$ garage_agentc chat agents/garage-menu/agent.txtpb
bot: Hi, this is Acme Garage. Would you like to book a service, hear our hours, or talk to a person?   [main/welcome]
     choices: Book a service | Opening hours | Talk to a person   (#1 taps the first)
     expects: CHOICE
you: I'd like to bring my car in
bot: Sure. Is it for an oil change or a tire repair?   [main/booking]
     choices: an oil change | a tire repair   (#1 taps the first)
     expects: SLOT service
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

  Expectations: `page`, `flow`, `said` (the whole reply), `choices` (the labels offered, in order), `said_contains` (case-insensitive), `matched`, `expects_reply`, `ends_session`, `session { key: "service" value: "an oil change" }` (a param as a reply would say it), `form { key: "day" value: "tuesday" }` (a slot, or `complete`/`confirmed`/`failed`; `""` when empty), `entities { key: "service" value: "oil_change" }` (`$nlu.entities`), and `event` (`no-match`, `no-input`, `max-reprompts`, `tool-error`, `clarify`). A turn with `no_input: true` is silence instead of words. `fire_trigger: "car-ready"` (with `trigger_payload`) fires a trigger instead, and `trigger_decision: QUEUED` checks what became of it. `after_ms` moves the clock on before a turn, from the case's `start_time` (local, `"2026-11-09T08:00:00"`). A turn's `tool_results` stand in for tools, and `tools_called` checks which ran. A case can set session params and context before the first turn: `session { fields { key: "tier" value { string_value: "gold" } } }`, `context { … }`. The same runs from C++ with `va::flow::runFlowTest`, and the example's cases run in `ctest` (`vasdk_agent_garage_menu`).
- **`chat`** talks to the agent by typing, showing the page (and any event) after each reply, the choices offered, and the parts that can't be interrupted. An empty line is silence (no-input). `--session tier=gold` sets a session param first.
- **`eval`** measures understanding on a set of [`NluEvalSet`](../../VASDK/proto/vasdk/flow/test_case.proto) utterances: `cases { text: "how much is an oil change" intent: "price" entities { key: "service" value: "oil_change" } }`. It reports intent accuracy, precision, recall and F1 per entity, and each miss. `--min-accuracy 0.9` fails below it, for CI; the example's set runs in `ctest` (`vasdk_agent_garage_menu_nlu`). A case with `mid_chat: true` was said mid-chat with the LLM, and is judged as the agent judges it then. `--sweep chat_min_share=0.15,0.25,0.5` (also `keyword_words`, `min_confidence`; several flags make a grid) prints the accuracy at each value of the engine config's `understanding` settings, to choose them by measurement; with `--llm`, the LLM is asked once per sentence. To build a set from real speech, run `VASDK_Pipeline --record-turns turns.txtpb`: each user turn is appended as STT heard it, with the agent's intent and `mid_chat`, for you to correct. Without recordings, `vasdk_spoken_eval <models> nlu_eval.txtpb heard.txtpb [voices] [stt]` says each case in local voices (Phonon's) and writes the set again as a local recognizer (Whistle) heard it; synthetic voices are cleaner than callers, so it's a floor for what speech costs. The garage set's is [`nlu_eval.spoken.txtpb`](../../VASDK_APP/agents/garage-menu/nlu_eval.spoken.txtpb), in `ctest` too.
- **With an LLM.** `--llm` adds one after the rules to `test`, `chat`, `try` and `eval`: its endpoint, model and key come from `LLM_ENDPOINT`, `LLM_MODEL` and `LLM_API_KEY` (or `OPENROUTER_API_KEY`, `OPENAI_API_KEY`). `--llm-endpoint <url>`, `--llm-model` and `--llm-api-key` give them instead. Any OpenAI-compatible chat completions URL works: OpenAI, OpenRouter, a local LM Studio, llama.cpp server or Ollama. In `chat`, a turn the LLM understood shows `understood by: llm (book)`. Without the flag the LLM isn't used, even when the variables are set, so `ctest` stays the same everywhere.
- **With models.** `--engine-config <file>` takes an engine config's `sentence_encoder`, `entity_model` and `understanding` settings, as a session would, for `test`, `chat`, `try` and `eval`; `eval` names what it used (`stages exact rules, minilm; entities by gliner`). `minilm` and `gliner` need a tool built with the providers: `garage_agentc` passes them (`runAgentTool(argc, argv, registry, &providers)`), `vasdk_agentc` doesn't. `entity_model { id: "llm" }` needs `--llm` too.
- **`try`** shows what a sentence matches, without running the conversation:

```
$ garage_agentc try agents/garage-menu/agent.txtpb "can I book a lube for the afternoon"
stage: rules
intent: book (score 1.00, confidence 1.00)
@service = oil_change  spoken "an oil change"  heard "lube"  price=49  minutes=30
@available_time = 14:00  spoken "2 PM"  heard "afternoon"
$ garage_agentc try agents/garage-menu/agent.txtpb --entity @sys.phone "five five five, one two three, double four six seven"
@sys.phone = 5551234467  heard "five five five one two three double four six seven"
```

## In C++

| Type | Use |
|---|---|
| `flow::CompiledAgent` | `load(path, error, warnings, registry)` or `compile(proto, ...)`: checked once, immutable, shared by every session that runs it. |
| `flow::FlowRegistry`, `TurnContext` | Steps, page types, envelopes and tools written in C++: see [Extensions](#extensions-page-types-and-c). `TurnContext::apply(StepOutcome)` for steps written in other languages. |
| `flow::ToolRunner` | Runs an agent's tools within an engine config's `security`; `Session` makes one. `maskValue`, `redact` mask sensitive values. |
| `flow::runAgentTool` | `vasdk_agentc`'s commands, for an app's own copy with its registry, and the providers `--engine-config` may name (`runAgentTool(argc, argv, registry, &providers)`). |
| `flow::FlowRuntime` | One conversation's place in an agent: `start()`, `handleText(text)` and `handleNoInput()` → `Reply` (text, its segments, choices, ui, matched, intent, event, expects reply, ends session, page changes, tool calls, triggers), `fireTrigger(name, payload)`, `deliverQueued()`, `setClock`, `replyInterrupted()`, `noInputMs()`, `setSessionParam`, `setEntityValues`, `setContextSource`, `setEntityExtractor` (an [entity model](#entity-models)), `setExactRules`. Copyable, way back included. |
| `flow::Expression` | `parse(text)` once, then `evaluate(lookup)` or `test(lookup)`; `renderTemplate` for replies. |
| `flow::DialogProcessor` | The pipeline stage `Session` adds for `Options::agent`, after the turn assembler. |
| `flow::runFlowTest` | Plays a `FlowTestCase` and returns what differed. |
| `flow::extractSlotValue`, `extractYesNo` | How a form reads a slot's value from an answer. |
| `flow::EntityCatalog` | `agent->entities()`: `find(name, text)`, `findAll(text)`, `describe(name, value)` → `EntityMatch` (value, spoken, raw, attributes). |
| `goal::GoalState` | A form's values: `updateSlot`, `clearSlot`, `missingSlots`, `isConfirmed`. Shared with the LLM goal path (`GoalManager`). |

## In other languages

Steps, tools, envelopes, page types and entity extractors can be written in C, and so in any language that calls C, with [the C API](c-api.md). The [Python binding](../../bindings/python/README.md) is built on it. On Android, Kotlin registers steps and tools through JNI (`VasdkEngine.registerAgentStep`, see [the Android app](../apps/android.md#an-agent-before-the-llm)).
