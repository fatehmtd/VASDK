# VASDK_Pipeline

The general-purpose voice assistant: mic to speaker with barge-in, turn detection, tool calling, metrics, a terminal UI and browser calls over WebRTC. With `--agent`, an agent file answers what it understands before the LLM.

**The full guide: [docs/apps/pipeline.md](../docs/apps/pipeline.md).**

```bash
./build/VASDK_APP/VASDK_Pipeline                                                     # cloud LLM from .env
./build/VASDK_APP/VASDK_Pipeline --llm-model-path build/VASDK_3P_IMPL/models/<model>.gguf   # fully local
./build/VASDK_APP/VASDK_Pipeline --config VASDK_APP/config/engine.txtpb \
  --agent VASDK_APP/agents/garage-menu/agent.txtpb --env-file .env --webrtc-port 8080   # an agent, in the browser
```

| Path | |
| --- | --- |
| [`main_pipeline.cpp`](main_pipeline.cpp) | The app: audio I/O, demo tools, recording, visualisers, the terminal UI around `VAEngine` |
| [`config/engine.txtpb`](config/engine.txtpb) | Providers, turn-taking, the LLM (and commented LLM profiles, sentence encoder and entity model) |
| [`agents/garage-menu/`](agents/garage-menu/) | The example agent, its C++ extensions and its tests |
| [`prompts/`](prompts/) | The system prompt and response format |
