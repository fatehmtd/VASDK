---
name: vasdk-app
description: Put a VASDK use case in place - pick its shape (an agent in an existing app, a vertical in the car app, or a new app on desktop, Python or Android), wire providers through an engine config, register its tests, document it, and ship the licenses of every integration and model it uses. Use when building a new assistant, demo, product or vertical on VASDK.
---

# A use case: agent, vertical or app

## Pick the smallest shape that does the job

| The use case is… | Shape | Start from |
| --- | --- | --- |
| a conversation an existing app can host | an agent | [vasdk-agent](../vasdk-agent/SKILL.md) |
| a new domain in the car (parking, charging, …) | a car vertical | `VASDK_CAR_APP/verticals/climate/` |
| a product with its own UI, devices or deployment | a new app | the closest app in [docs/apps](../../docs/apps/README.md) |
| something the SDK can't do yet | core work first | [vasdk-core-component](../vasdk-core-component/SKILL.md), then come back |

An app holds what is specific to it: its agent, its tools, its UI, its config. A capability two apps could use goes in the core ([house rules](../../AGENTS.md#house-rules)).

## A car vertical

A vertical is self-contained in `VASDK_CAR_APP/verticals/<name>/`, as `climate/` is:

| File | What |
| --- | --- |
| `<Name>.h`, `<Name>.cpp` | its code: state, tools, triggers (see `VASDK_CAR_APP/core/Vertical.h`) |
| `<name>.txtpb` | its part of the agent |
| `<name>_test.cpp` | its code tested against the agent (`vasdk_car_<name>_code`) |
| `tests/*.txtpb` | its conversations (`vasdk_car_<name>`) |
| `panel.js` | its dashboard panel |

Then, for a vertical called `Parking`: `parking` in `CAR_VERTICALS` in `VASDK_CAR_APP/CMakeLists.txt`, `car->add<Parking>();` in `VASDK_CAR_APP/core/Verticals.cpp`, its `.txtpb` in `include_files` of `VASDK_CAR_APP/agents/car/agent.txtpb`, and a section in `docs/apps/car.md`. Its tests are picked up from the folder layout.

## A new app

1. Copy the layout of the closest app: `VASDK_APP/` (general assistant, `main_pipeline.cpp`), `VASDK_CALLCENTER_APP/` (agent-first, phone calls), `VASDK_CAR_APP/` (verticals, dashboard), `VASDK_PYTHON_APP/` (Python), `VASDK_ANDROID/` (Kotlin over JNI). Keep: `CMakeLists.txt`, `config/engine.txtpb` (providers chosen by id, secrets as `${ENV}`), `agents/`, `prompts/`.
2. Pick providers in `config/engine.txtpb`, not in code ([engine config](../../docs/reference/engine-config.md)), with a `fallback` for every cloud provider.
3. Root `CMakeLists.txt`: `option(VASDK_BUILD_<APP> "..." ON)` and `add_subdirectory(<APP_DIR>)`, as the car app is.
4. Docs: `docs/apps/<name>.md` (what it shows, how to run it, how it's built, with a mermaid diagram of its pipeline), a row in `docs/apps/README.md` and in the README's app table.
5. **Licenses**: below.

## Licenses ship with the app

An app carries the license of every integration and model it uses, so whoever ships it can see what they agreed to.

1. List what it uses: the `vasdk_*` targets its CMakeLists links (and the `VASDK_BUILD_*` options it needs), the provider ids in its `engine.txtpb` and their fallbacks, the models it downloads, its LLM service, and any library it adds itself.
2. For each, take the row from the root `THIRD_PARTY_NOTICES.md` (source and license).
3. Write `<app>/THIRD_PARTY_NOTICES.md` from [app-notices-template.md](app-notices-template.md): VASDK's own Apache 2.0 license, then one row per library and model with its license and a direct link to the license text. Or copy each license file into `<app>/licenses/<name>.txt` and list them. Either way every entry is reachable in one click.
4. Restrictive terms go at the top: non-commercial models (Phonon's default CC BY-NC voices), use-restricted ones (OpenRAIL: Supertonic), and cloud services' terms of use.
5. Link it from the app's doc page. An Android app also ships it in the APK (assets) and shows it in its about screen; a desktop app installs it next to the binary.

## Tests

How to write, label and register them: [vasdk-testing](../vasdk-testing/SKILL.md).

- **Unit:** the app's own code (tools, verticals, adapters): plain test executables registered under `if(VASDK_BUILD_TESTS)`, like `vasdk_car_<name>_code`.
- **Integration:** its agent's conversations through `<app>_agentc test` (like `vasdk_agent_garage_menu`), and a spoken run where the models are present (skips otherwise), like `vasdk_offline_conversation`. Python apps: `VASDK_PYTHON_APP/CMakeLists.txt` registers `vasdk_python_kitchen` and `vasdk_python_session`.

## Done when

The [definition of done](../../AGENTS.md#definition-of-done) holds; the app builds and runs on the platforms it claims (others are named, with the reason), its tests run in `ctest`, it has its doc page, and its notices list every integration and model it uses.
