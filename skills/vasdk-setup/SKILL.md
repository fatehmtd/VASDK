---
name: vasdk-setup
description: A setup wizard for VASDK apps - asks the user, one question at a time, for the language, the app's shape, Python or not, the providers (VAD, STT, TTS, turn, sentence encoder, entity model) with their options, the LLM, the pipeline (turn-taking, activation, audio), the agent's entities (system, packs, custom) and its flows, pages and turns, then writes the engine config, the agent and the app's files and checks them. Also opens an existing app or agent and changes only what the user picks. Use when someone wants to start a new assistant, set up or reconfigure an app, switch providers, or add an entity, a flow or a page.
---

# The setup wizard

A new app or a change to an existing one, by asking. The user answers questions; you read the choices from the code, write the files and check them. The details stay in the stage skills and in [docs/](../../docs/README.md); this skill is the order of the questions.

## How to ask

- **One step at a time.** Use the host's question tool when it has one (AskUserQuestion in Claude Code), otherwise a numbered question in plain text. At most 4 options a question, the recommended one first, and say what skipping gives (the default).
- **Read the choices, don't remember them.** Provider ids: `VASDK_3P_IMPL/ProviderCatalog/ProviderCatalog.cpp`. Their options: `VASDK_3P_IMPL/ProviderCatalog/proto/vasdk/providers/options.proto`. Everything else in the config: `VASDK/proto/vasdk/engine/engine_config.proto`. The agent: `VASDK/proto/vasdk/flow/agent.proto`, `VASDK/proto/vasdk/nlu/entity.proto`. Read them when the step comes up.
- **Explain before asking.** One or two lines on what the choice does and what it costs (size, network, license), from the provider's doc page in `docs/providers/`.
- **When files are written.** The config and the app's files (`engine.txtpb`, CMake, `.env.example`, docs) only after the user confirms the summary (step 12). The agent is written piece by piece in steps 9 and 10, so `lint` checks each piece; confirm its folder before the first write.
- **Secrets never pass through the chat.** Keys are written as `${NAME}` in the config; the user puts the value in `.env`. Don't ask for a key's value.

## Steps

**0. New or existing.** For an existing app (`VASDK_APP`, `VASDK_CALLCENTER_APP`, `VASDK_CAR_APP`, `VASDK_PYTHON_APP`), read its `config/engine.txtpb` and its agent, show what they choose as a table, and ask which parts to change: providers, LLM, pipeline, entities, conversation. Go through those steps only, edit in place, and keep the comments. The language is the agent's `locale`.

**1. Language, first.** Ask the agent's `locale` and any other `languages` its users speak. It decides what later steps offer:
- VASDK ships one language pack, `VASDK/languages/en-US.txtpb`. Another language needs a pack in the agent's `language_files`: offer to start one from it ([languages](../../docs/reference/languages.md), [worked example](../../docs/examples/languages.md)).
- Step 5 offers only the STT and TTS whose doc page lists the language, and sets their `language` option (Whistle's `LocalSttOptions`, Phonon's and Supertonic's `LocalTtsOptions`). The recognizer's language must match the locale ([vasdk-stt](../vasdk-stt/SKILL.md)).
- MiniLM and the `tagger` entity model are English only; otherwise recommend `gliner` built with `-DVASDK_GLINER_MODEL=multi`, or the `llm` entity model.

**2. Shape and name.** From [vasdk-app](../vasdk-app/SKILL.md#pick-the-smallest-shape-that-does-the-job): an agent in an existing app, a car vertical, or a new app (desktop C++ or Android). Then the platforms it must run on.

**3. Python?** Ask whether they want to drive or extend it from Python. If yes:
- lay it out like `VASDK_PYTHON_APP/` (`app.py`, `pyproject.toml`, `config/`, `agent/`, `tests/`);
- its tools, steps, page types and code entities are Python functions on a `vasdk.Registry` (`@registry.tool`, `@registry.entity_extractor`, `registry.add_language`; [binding README](../../bindings/python/README.md)) instead of C++;
- its tests are registered as `vasdk_python_kitchen` and `vasdk_python_session` are, in `VASDK_PYTHON_APP/CMakeLists.txt`; install with the binding README's uv or pip steps.

If no, C++ extensions and a `<name>_agentc` tool, as in `VASDK_APP/agents/garage-menu/extensions/`.

**4. Profile.** Offline, cloud with local fallbacks, or cloud only. It picks the recommended option of every provider question.

**5. Providers**, one question per stage ([engine config](../../docs/reference/engine-config.md#providers)), then that stage's options:

| Stage (`providers.`) | Options message | Worth asking |
| --- | --- | --- |
| `vad` | `SileroOptions` | `threshold` (lower hears quieter speech) |
| `stt` | `LocalSttOptions` / `CloudOptions` | `language`; cloud: `api_key` as `${<PROVIDER>_API_KEY}` |
| `tts` | `LocalTtsOptions` / `CloudOptions` | `voice`, `language`; cloud: `api_key`, `voice_id` |
| `turn` | `Turn1MiniOptions` (`turn-1-mini`, English), `SmartTurnOptions` (`smartturn-v3`, any language) | whether to use one (without it, the VAD's silence ends turns), and which: `turn-1-mini` for English, `smartturn-v3` otherwise |
| `sentence_encoder` | `MiniLmOptions` | whether the agent should understand paraphrases its examples miss |
| `entity_model` | `GlinerOptions` (`gliner`); none for `tagger`, `llm` | `threshold` ([which one](../../docs/reference/flows.md#entity-models)) |
| `wake_word` | | none ships yet: say so and point to [vasdk-wake-word](../vasdk-wake-word/SKILL.md) |
| `webrtc` | `LibDataChannelWebRtcOptions` (`libdatachannel`) | whether a browser brings the audio instead of the app; `port`. Not with SonioxPP in the same build ([webrtc](../../docs/providers/webrtc.md#options)) |

Every cloud provider gets a local `fallback`. Check each choice's doc page against the platforms from step 2 and say what isn't supported where. The stage skill has the rest ([vasdk-stt](../vasdk-stt/SKILL.md), [vasdk-tts](../vasdk-tts/SKILL.md), [vasdk-vad](../vasdk-vad/SKILL.md), [vasdk-turn-detection](../vasdk-turn-detection/SKILL.md), [vasdk-nlu](../vasdk-nlu/SKILL.md)).

**6. LLM** ([vasdk-llm](../vasdk-llm/SKILL.md)): none, llama.cpp on the device, an OpenAI-compatible endpoint, or Anthropic (`api: ANTHROPIC`). Then `endpoint`, `model`, `api_key` as `${ENV}`, `effort`. Offer a small `llm_profiles` entry for the agent's intent check (`understanding.llm_profile`).

**7. Pipeline** ([turn-taking](../../docs/reference/engine-config.md#turn-taking), [activation](../../docs/reference/engine-config.md#activation)):
- `turn_taking.preset`: BALANCED, SNAPPY (phone calls), PATIENT (people who pause); `streaming_transcription`; `backchannel.pending_playback` (DUCK, PAUSE, CONTINUE).
- `activation.mode` (ALWAYS_ON, PUSH_TO_TALK; WAKE_WORD and HYBRID need a wake-word provider) and `follow_up.listen_ms`.
- `audio`: the defaults, or 8 kHz in and out for phone lines.
- `remote_control`: off; on, with `token: "${VASDK_RC_TOKEN}"` and OBSERVE unless they need CONTROL.

**8. Agent.** None (the LLM answers everything), a new one laid out like `VASDK_APP/agents/garage-menu/`, or an existing one. Its tools that call HTTP need `security.http_allow` and `security.secrets` in the config ([security](../../docs/reference/engine-config.md#security)).

**9. Entities** and **10. Flows, pages and turns**: the loop in [conversation.md](conversation.md). It runs until the user says they're done, and comes back to step 9 whenever a turn needs an entity that isn't declared yet.

**11. Build.** From each chosen provider's doc page: the `-DVASDK_BUILD_<NAME>=ON` options and the models it downloads ([building](../../docs/building.md)).

**12. Summary.** Show the whole `engine.txtpb`, every file to create or change, the CMake command, and the `.env` names to fill in. Change anything the user asks, then confirm.

**13. Write.**
- `config/engine.txtpb`, starting with the `# proto-file: vasdk/engine/engine_config.proto` and `# proto-message: vasdk.engine.EngineConfig` lines, as `VASDK_APP/config/engine.txtpb` does, and a comment by each choice saying why.
- The `.env` names, without values, in `.env.example`.
- A new app: CMake, doc page, README rows and licenses as [vasdk-app](../vasdk-app/SKILL.md#a-new-app) lists them ([licenses](../vasdk-app/SKILL.md#licenses-ship-with-the-app)).

**14. Check.** Build with the options from step 11; `vasdk_agentc lint` and `test` on the agent (with `--engine-config` when a sentence encoder or entity model is set; an agent with C++ extensions uses its own `<name>_agentc`); run the app once with `--config` and `--env-file .env`; `ctest`. Report each as passed, failed or skipped. Then the [definition of done](../../AGENTS.md#definition-of-done).

## Done when

The user has confirmed the summary, every file it named is written, `lint` has no errors, the agent's tests and `ctest` pass, and the user has said they're done with the conversation loop.
