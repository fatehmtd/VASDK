# VASDK_CallCenter

A phone call answered by an agent file: a routing page, two forms with read-backs, CRM tools and a handoff to a person, with no LLM needed. With one, it also understands what its examples miss.

**The full guide: [docs/apps/callcenter.md](../docs/apps/callcenter.md).**

```bash
./build/VASDK_CALLCENTER_APP/VASDK_CallCenter --ani 555-1234      # "I need to schedule a repair for my car"
./build/VASDK_CALLCENTER_APP/callcenter_agentc test VASDK_CALLCENTER_APP/agents/callcenter/agent.txtpb \
  VASDK_CALLCENTER_APP/agents/callcenter/tests/*.txtpb             # the calls, by text
```

| Path | |
| --- | --- |
| [`agents/callcenter/`](agents/callcenter/) | The call flow, its goals and its test calls |
| [`crm/`](crm/) | The CRM tools, where your systems plug in |
| [`callcenter_pipeline.cpp`](callcenter_pipeline.cpp) | The app: one engine session per call, phone-call turn-taking, the call log |
| [`prompts/`](prompts/) | The LLM's instructions for what the agent doesn't cover |
