---
name: vasdk-testing
description: Write, label and register any VASDK test - C++ (GoogleTest through vasdk_add_gtest, the shared support in VASDK/tests/support), Python (pytest), Kotlin, agent suites, evaluation runs and scripts - so every test looks and behaves the same way, waits instead of sleeping, skips cleanly without models or keys, and runs side by side. check_tests.py (ctest vasdk_test_rules) enforces it. Use before writing or changing a test in any language.
---

# Testing in VASDK

One way to write a test, whatever it checks: the layers and labels, the framework per language, the shared helpers, and a checker that fails `ctest` when a test strays. Background, and how to run a subset: [docs/testing.md](../../docs/testing.md).

## Layers and labels

Every test has exactly one label, its layer. `vasdk_test_labels` fails on a test without one.

| Label | What it checks | Written with | Template |
| --- | --- | --- | --- |
| `unit` | deterministic logic: parsing, scoring, features against golden data | GoogleTest | `VASDK/tests/expression_test.cpp` |
| `timing` | threads and real time: turns, transports, streaming; registered `RUN_SERIAL` | GoogleTest and `va::test::waitUntil` | `VASDK/tests/tts_turn_test.cpp`, `VASDK/tests/webrtc_transport_test.cpp` |
| `integration` | a session, the C API, the engine with fakes, the catalog | GoogleTest | `VASDK_3P_IMPL/ProviderCatalog/offline_conversation_test.cpp` |
| `model` | a provider on its downloaded model; skipped without it | GoogleTest and `VA_REQUIRE_FILE` | `VASDK_3P_IMPL/MiniLmNlu/minilm_nlu_test.cpp` |
| `agent` | an agent's scripted conversations | `.txtpb` cases run by `<app>_agentc test` | `VASDK_APP/agents/garage-menu/tests/book_service.txtpb` |
| `eval` | quality on real speech against `baselines.txtpb` | `vasdk_provider_eval` | [Evaluation](../../docs/evaluation.md) |
| `binding` | Python, Kotlin and C over the C library | pytest, kotlin.test, C | `bindings/python/tests/test_score.py`, `bindings/kotlin/src/test/kotlin/com/vasdk/ScoreTest.kt`, `VASDK/tests/vasdk_c_test.c` |
| `packaging` | the consumer builds, model downloads, the checkers | CMake scripts, Python | `tests/consumer/run_consumer_test.cmake` |

## A C++ test

- **File and target:** `<feature>_test.cpp` next to what it tests (`VASDK/tests/`, `VASDK_3P_IMPL/<Name>/tests/`), registered in that folder's CMakeLists under `if(VASDK_BUILD_TESTS)`:

  ```cmake
  vasdk_add_gtest(vasdk_<feature> LABEL unit SOURCES tests/<feature>_test.cpp LIBRARIES VASDK)
  vasdk_add_gtest(vasdk_<name> LABEL model SOURCES tests/<name>_test.cpp LIBRARIES vasdk_<name> MODELS_DIR ${<NAME>_MODEL_DIR})
  ```

  `vasdk_add_gtest` (`cmake/VASDKTesting.cmake`) links GoogleTest, gMock and the shared support, copies the Windows DLLs, and makes each case its own ctest test, `vasdk_<feature>.<Suite>.<Case>`. Options: `TIMEOUT`, `RUN_SERIAL` (every `timing` test), `MODELS_DIR`, `ARGS`, `WORKING_DIRECTORY`. Never a bare `add_executable` + `add_test`.
- **Names:** `TEST(<Component>, <BehaviourInPascalCase>)`: the suite names what is tested, the case says what it does (`TtsTurn.PauseHoldDelaysCompleted`). A shared setup is one fixture, `<Component>Test`, with `TEST_F`; modes are `TEST_P` with a name generator (`Queued`, `OneAtATime`). PascalCase, no underscores.
- **Assertions:** `EXPECT_EQ`, `EXPECT_GE`, `EXPECT_NE` over `EXPECT_TRUE`, so a failure shows both values. `ASSERT_*` only to guard what follows (a null pointer, a failed `initialize()`). Each says what was expected: `<< "COMPLETED only after the last sentence played"`. A type a failure can't print gets a `PrintTo` (as `SynthesisState` in `VASDK/tests/tts_turn_test.cpp`); durations are compared as millisecond counts.
- **Time** (`VASDK/tests/support/TestSupport.h`):
  - Wait for anything that should happen: `va::test::waitUntil(condition, timeout)`, which returns as soon as it holds. A loaded machine (a shared CI runner) runs late, never early.
  - A fixed `sleep_for`/`sleep_until` is only for time that is part of the scenario (the user cuts in 50 ms into a sentence, a microphone's 100 ms frames) or for checking that something does *not* happen. It says so on its line: `// fixed wait: <why>`.
  - An upper bound on a duration is measured from the time that really passed (the pacing check in `VASDK/tests/webrtc_transport_test.cpp`), never assumed from a sleep.
  - A scenario an oversleeping machine can't stage skips: `GTEST_SKIP() << "the machine overslept"`.
- **Files and state:** temporary files go in a `va::test::UniqueTemporaryDirectory`, removed with the test. No globals and no `static` variables: state lives in the fixture, so `ctest -j` and `--gtest_shuffle` are safe. Members a callback reaches (a mutex, a list it fills) are declared before what calls it, so they outlive it.
- **Skips:** `VA_REQUIRE_FILE(va::test::modelsDirectory() / "model.onnx")` for a model or data file, `GTEST_SKIP() << "<what is missing>"` otherwise (a provider not built, no `*_API_KEY`). ctest reports them as skipped, not passed.
- **Fakes:** shared ones go in `VASDK/tests/support/` (`VASDK/tests/engine_test_fakes.h` moves there as its tests are converted). gMock (`MOCK_METHOD`, `EXPECT_CALL`) when the test checks the calls made; a hand-written fake when it checks behaviour over time.

## Python, Kotlin, agents and scripts

- **Python:** pytest functions (`def test_...`), the library and the models from `conftest.py` fixtures, `pytest.skip` when something is missing; registered with `vasdk_add_pytest`. Being converted: see below.
- **Kotlin:** `kotlin.test` in `bindings/kotlin/src/test/`, run by `vasdk_kotlin` (`gradle test`) and on a device as instrumented tests ([Android](../../docs/apps/android.md)).
- **Agents:** one `.txtpb` per behaviour in the agent's `tests/` folder ([vasdk-agent](../vasdk-agent/SKILL.md)); `<app>_agentc test` runs them, labelled `agent`.
- **Scripts and CMake tests:** exit 77 to skip, with `SKIP_RETURN_CODE 77`, and a label through `vasdk_label_tests(<label> <test>)`.

## The checker

`check_tests.py` (`vasdk_test_rules`, and `vasdk_test_rules_self_test` for the checker itself) fails on a C++ test with a `main()`, its own `check()`/`waitUntil()`/failures counter, `return 77`, a fixed temporary folder name, a sleep without `// fixed wait:`, a name not in PascalCase, or a registration other than `vasdk_add_gtest` with a `LABEL`; and on a Python test that isn't pytest. Run it by hand with `python3 skills/vasdk-testing/check_tests.py`.

Tests written before GoogleTest are listed in its `NOT_YET_CONVERTED` until their phase converts them ([docs/testing.md](../../docs/testing.md#the-migration)). The list only shrinks: converting a file takes it off and lowers `MIGRATION_CEILING`; a new test never goes on it.

## Done when

- The [definition of done](../../AGENTS.md#definition-of-done) holds: a unit test, an integration test, skips rather than failures without models or keys, `ctest` reported as passed, failed or skipped.
- The test has its label, and `vasdk_test_rules` and `vasdk_test_labels` pass.
- It passes 20 times in a row, shuffled: `<test program> --gtest_repeat=20 --gtest_shuffle`; a `timing` test also with the rest of the suite running beside it (`ctest -j`).
