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

Agents are text proto ([`agent.proto`](../../VASDK/proto/vasdk/flow/agent.proto)); JSON and binary load too. This is a trimmed copy of the [example](../../VASDK_APP/agents/garage-menu/agent.txtpb):

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
| `flows` | Named groups of pages. `target: "flow:handoff"` goes to another flow's start page. |
| `pages` | `on_enter` steps run each time the conversation enters the page; `routes` are tried against each user turn while it's there. |
| `routes` | `intent` to match, `steps` to run, then `target`: a page of this flow, `flow:<name>`, `CURRENT_PAGE` (enter this page again) or `END_SESSION`. No target: stay on the page without entering it again. |

Steps:

| Step | Example | What it does |
|---|---|---|
| `say` | `say { text: ["Got it.", "Okay."] }` | Speaks one text; with several, a different one each time the step runs. |
| `ask` | `ask { say { text: "Which day?" } }` | Speaks a question: the reply expects an answer (`agent_reply.expects_reply`), and the question is repeated when the answer isn't understood. |
| `goto` | `goto { target: "confirm" }` | Moves to the target at once; the steps after it don't run. |
| `end` | `end {}` | Ends the conversation once the reply has played. |

Everything said in one turn is joined into one reply: a route's steps, then the `on_enter` steps of every page entered.

## How a turn is answered

1. **Matching.** Every route on the current page is scored by the longest example of its intent found in the user's words. The best score wins; on a tie, the first route. "cancel my appointment" picks `cancel` (three words) over `book` ("appointment", one word).
2. **Not understood.** No route matched: the reply is "Sorry, I can't help with that here." and the page's last question. With an LLM, the turn goes to the LLM instead.
3. **Start.** The start page's `on_enter` (the greeting) runs once the assistant is listening (at start when always on, else on the wake word or button) and someone is connected to hear it: a browser peer over WebRTC, a caller. With the local mic that's at once. If the user speaks first, their words are routed on the start page without the greeting. When the peer or caller disconnects, the next one starts a new conversation with its own greeting.
4. **End.** After an `end` step or `END_SESSION`, the conversation starts over: the greeting plays on the next activation, or, when always on, the next turn is routed on the start page.
5. **Resumed turns.** When the user carries on talking after a turn was answered (`UserTurnFrame::replacesPrevious`), the agent goes back to where it was before that turn and routes the whole sentence.
6. **Loops.** A turn that enters more than 8 pages (a `goto` loop) stops on the page it reached, with an error in the log.

## Events

With an agent, listeners also get `page_changed` (`from_flow`/`from_page` → `to_flow`/`to_page`, with the reason `start`, `restart`, `route:<intent>` or `goto`) and `session_ended`. See [VAEngine events](engine.md#events).

## Checking and testing: `vasdk_agentc`

```
$ vasdk_agentc lint agents/garage-menu/agent.txtpb
agents/garage-menu/agent.txtpb: ok (2 flows, 4 pages, 9 intents)

$ vasdk_agentc test agents/garage-menu/agent.txtpb agents/garage-menu/tests/*.txtpb
PASS book a tire service, then say goodbye
PASS asking for a person hands off and ends
PASS an answer that isn't understood repeats the question
3 of 3 passed

$ vasdk_agentc chat agents/garage-menu/agent.txtpb
bot: Hi, this is Acme Garage. Would you like to book a service, hear our hours, or talk to a person?   [main/welcome]
you: I'd like to bring my car in
bot: Sure. Is it for an oil change or tires?   [main/service]
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

  Expectations: `page`, `flow`, `said` (the whole reply), `said_contains` (case-insensitive), `matched`, `expects_reply`, `ends_session`. The same runs from C++ with `va::flow::runFlowTest`, and the example's cases run in `ctest` (`vasdk_agent_garage_menu`).
- **`chat`** talks to the agent by typing, showing the page after each reply.

## In C++

| Type | Use |
|---|---|
| `flow::CompiledAgent` | `load(path)` or `compile(proto)`: checked once, immutable, shared by every session that runs it. |
| `flow::FlowRuntime` | One conversation's place in an agent: `start()`, `handleText(text)` → `Reply` (text, matched, intent, expects reply, ends session, page changes). Copyable. |
| `flow::DialogProcessor` | The pipeline stage `Session` adds for `Options::agent`, after the turn assembler. |
| `flow::runFlowTest` | Plays a `FlowTestCase` and returns what differed. |

## Not yet

These come in later milestones: conditions and stored values (`$session.*`, branching), forms that collect several values, custom entities, global commands and digressions that return where you were (`END_FLOW`), handlers for no-input and repeated misunderstandings, rich replies, and custom steps written in C++.
