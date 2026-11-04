# VASDK_CallCenter: a call center in agent files

`VASDK_CallCenter` answers a phone call from an [agent file](agents/callcenter/agent.txtpb): a routing page, two forms, CRM tools and a handoff to a person, with no LLM needed. The call runs as an [engine session](../docs/reference/engine.md#many-conversations-at-once), the same engine as [`VASDK_Pipeline`](../VASDK_APP/README.md), with phone-call turn-taking. [`callcenter_pipeline.cpp`](callcenter_pipeline.cpp) is the app; [`agents/callcenter/`](agents/callcenter/) is the call flow; [`crm/`](crm/) holds the tools.

## Call flow

```mermaid
flowchart TD
    ANI["caller's number (ANI)"] --> W["welcome: lookup_account<br/>'Hi Alice, ... what do you need?'"]
    W -->|"schedule a repair"| S["schedule form<br/>name and number from the CRM,<br/>service, vehicle (repairs only), a future date"]
    S -->|"read back, yes"| T["schedule_callback tool"] --> D["'Your reference is CB-1234'"]
    W -->|"my account"| A["account form<br/>the caller's own, or the id they say"] --> L["lookup_account"] --> R["status"]
    W -.->|"a person, or too many misunderstandings"| H["handoff: the call ends, the app transfers it"]
    S -.-> H
    L -.->|"not found"| H
```

## What it demonstrates

- **Routing page**: the caller's first words go to a form by their intents' examples ("I need to schedule a repair", "what's my account balance"). Words that also fill a slot fill it on the way in ("repair").
- **Forms**: a free-text name, a `PHONE` number, an `ENUM` service, a vehicle asked only when the service is a repair (a slot `condition`), and a future-only `DATE` ("next Tuesday"). The values are read back, and a "yes" schedules the callback. "No" asks what to change.
- **CRM tools**: `lookup_account` and `schedule_callback` are written in C++ ([`crm/CallCenterTools.cpp`](crm/CallCenterTools.cpp)), with a fixed table where your CRM plugs in. The caller's number fills their name, number and account before they're asked.
- **Handoff**: "talk to a person" works on every page, also while a free-text answer is expected. Too many misunderstandings in a row, or a tool that fails, hand off too.
- **An agent per DNIS**: `--dnis 8005550100` answers with `agents/8005550100/agent.txtpb` when there is one, else the default agent. A server compiles each once and shares it between the calls to that number.
- **Online and offline**: with no LLM the agent answers on its own. With one (`--llm-endpoint`, or `LLM_ENDPOINT` in `.env`) it also understands what its examples miss and reads answers its forms can't, and anything it doesn't cover goes to the LLM. If the network drops, the agent keeps the call going ([network fallback](../docs/reference/engine.md#when-the-network-drops)).
- **Error recovery**: a `RecoveryProcessor` catches STT/LLM/TTS `ErrorFrame`s, plays a retry prompt, and ends the call on repeated failure.

## Testing the call flow

The calls in [`agents/callcenter/tests/`](agents/callcenter/tests/) run by text, with no audio and no LLM, in `ctest`. They run twice: offline (`vasdk_agent_callcenter`), and with an LLM after the rules (`vasdk_agent_callcenter_with_llm`). The same suite passes both ways.

```bash
./build/VASDK_CALLCENTER_APP/callcenter_agentc test VASDK_CALLCENTER_APP/agents/callcenter/agent.txtpb VASDK_CALLCENTER_APP/agents/callcenter/tests/*.txtpb
./build/VASDK_CALLCENTER_APP/callcenter_agentc chat VASDK_CALLCENTER_APP/agents/callcenter/agent.txtpb --session caller=555-1234
# the same calls with your LLM after the rules (LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY)
./build/VASDK_CALLCENTER_APP/callcenter_agentc test VASDK_CALLCENTER_APP/agents/callcenter/agent.txtpb VASDK_CALLCENTER_APP/agents/callcenter/tests/*.txtpb --llm
```

## Running

```bash
./build/VASDK_CALLCENTER_APP/VASDK_CallCenter --ani 555-1234
```

Say something like "I need to schedule a repair for my car" and let it walk you through the form. The CRM knows `555-1234` (Alice Johnson) and `555-5678` (Bob Smith); any other number is a new caller.

## CLI flags

| Flag | Purpose |
| --- | --- |
| `--ani <phone>` | Caller ANI: the agent looks the caller up in the CRM |
| `--dnis <number>` | The number called: answered with `agents/<number>/agent.txtpb` when there is one |
| `--agent <path>` | An agent file to answer with, instead of the DNIS's |
| `--llm-endpoint <url>` | OpenAI-compatible LLM endpoint; unset (and no `LLM_ENDPOINT`): no LLM |
| `--llm-api-key <key>` | LLM API key (falls back to `LLM_API_KEY`/`OPENROUTER_API_KEY`/`OPENAI_API_KEY`) |
| `--llm-model <name>` | LLM model name (falls back to `LLM_MODEL`) |
| `--system-prompt <path>` | The LLM's instructions for what the agent doesn't cover ([`prompts/system_prompt.txt`](prompts/system_prompt.txt) is an example) |
| `--format-instruction <path>` | Override the response format instruction file |
| `--env-file <path>` | `.env`-style file to load (default: `.env`) |
| `--no-backchannel` | Back-channel handling off: any speech over the assistant interrupts it (default: on, "mm-hmm" and "yeah" don't interrupt) |
| `--backchannel-playback <mode>` | Playback while speech over the assistant is classified: `duck` (default, −12 dB), `pause` (resume in place) or `continue` |
| `--duck-gain-db <dB>` | Assistant level while ducked, −60 to 0 (default −12) |
| `--duck-duration-ms <ms>` | Minimum time the assistant stays ducked once speech over it starts (default 2500) |
| `--backchannel-history <mode>` | Dismissed back-channels: `drop` (default) or `append` them to the front of the user's next message |
| `--tail-overlap <mode>` | Speech over the last ~0.7 s of a reply: `turn` (default, answered after the reply) or `classify` (back-channels dismissed) |
| `--webrtc-port <port>` | Talk from a browser over WebRTC instead of the local mic/speaker: serves signaling and the demo page on `http://127.0.0.1:<port>` (only with `VASDK_BUILD_LIBDATACHANNEL_WEBRTC`) |
| `--webrtc-page <path>` | Page served at `/` in WebRTC mode (default: [`assets/webrtc/index.html`](../assets/webrtc/index.html)) |
| `--debug` | Enable debug logging |

## Production notes

The CRM here is a hardcoded in-memory table, and scheduled callbacks are kept in memory: both mark the seam where your real systems plug in ([`crm/CallCenterTools.cpp`](crm/CallCenterTools.cpp)). The handoff ends the call with a `session_ended` event; a telephony app transfers it there.

The earlier version of this app drove the call with the LLM goal layer (`IntentRouter` and `GoalManager`), and could resume a dropped call from saved goal state (`--persist-dir`). The agent version doesn't resume a dropped call yet. The goal layer is still in VASDK for apps built on it.
