#!/usr/bin/env python3
"""Checks that every test is written and registered the one way skills/vasdk-testing describes.

  python3 skills/vasdk-testing/check_tests.py              check this repository
  python3 skills/vasdk-testing/check_tests.py --root DIR   check another tree laid out the same way
  python3 skills/vasdk-testing/check_tests.py --self-test  run the checks on planted faults; each must be found

C++ tests (*_test.cpp):
  - GoogleTest cases (TEST, TEST_F, TEST_P), no main() of their own
  - no helpers of their own: check(), expect(), checkEq(), waitUntil(), waitFor(), a failures counter
    (VASDK/tests/support/TestSupport.h has the shared ones)
  - no `return 77`: GTEST_SKIP() or VA_REQUIRE_FILE
  - no temporary folder of a fixed name (temp_directory_path() / "..."): va::test::UniqueTemporaryDirectory
  - every sleep_for or sleep_until says why on its line: `// fixed wait: <why>`
  - suite and case names in PascalCase, without underscores
  - registered through vasdk_add_gtest, with a LABEL, never a bare add_executable
Python tests (test_*.py): pytest functions (def test_...), no sys.exit(main()), the library from
  conftest.py's fixtures rather than read from VASDK_LIBRARY; never a bare add_test.
Files written before the framework are listed in NOT_YET_CONVERTED until their phase converts
them. The list only shrinks: MIGRATION_CEILING must equal its length, and an entry that already
follows the rules must come off. Standard library only.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIPPED_FOLDERS = {".git", ".gradle", ".cxx", ".venv", "node_modules", "_deps", "__pycache__", "third_party"}

# Test files that predate GoogleTest and pytest. Each phase of the migration (docs/testing.md)
# takes the files it converts off this list and lowers MIGRATION_CEILING to match; nothing is added.
NOT_YET_CONVERTED = [
    "VASDK/tests/activation_test.cpp",
    "VASDK/tests/agent_include_test.cpp",
    "VASDK/tests/backchannel_test.cpp",
    "VASDK/tests/c_conversation_test.cpp",
    "VASDK/tests/c_providers_test.cpp",
    "VASDK/tests/c_session_test.cpp",
    "VASDK/tests/config_providers_test.cpp",
    "VASDK/tests/dialog_test.cpp",
    "VASDK/tests/embedding_cache_test.cpp",
    "VASDK/tests/engine_assembly_test.cpp",
    "VASDK/tests/engine_events_test.cpp",
    "VASDK/tests/entity_extractor_test.cpp",
    "VASDK/tests/entity_model_test.cpp",
    "VASDK/tests/entity_test.cpp",
    "VASDK/tests/envelope_layering_test.cpp",
    "VASDK/tests/envelope_steps_test.cpp",
    "VASDK/tests/expectation_test.cpp",
    "VASDK/tests/flow_navigation_test.cpp",
    "VASDK/tests/follow_up_window_test.cpp",
    "VASDK/tests/form_edge_cases_test.cpp",
    "VASDK/tests/form_primitives_test.cpp",
    "VASDK/tests/goal_test.cpp",
    "VASDK/tests/http_cancel_test.cpp",
    "VASDK/tests/language_test.cpp",
    "VASDK/tests/llm_assist_test.cpp",
    "VASDK/tests/llm_stream_speech_test.cpp",
    "VASDK/tests/model_store_test.cpp",
    "VASDK/tests/network_fallback_test.cpp",
    "VASDK/tests/page_type_test.cpp",
    "VASDK/tests/pipeline_description_test.cpp",
    "VASDK/tests/primitive_test.cpp",
    "VASDK/tests/proto_load_test.cpp",
    "VASDK/tests/provider_registry_test.cpp",
    "VASDK/tests/remote_control_test.cpp",
    "VASDK/tests/response_envelope_test.cpp",
    "VASDK/tests/rule_nlu_test.cpp",
    "VASDK/tests/scheduler_test.cpp",
    "VASDK/tests/security_test.cpp",
    "VASDK/tests/session_test.cpp",
    "VASDK/tests/speech_style_test.cpp",
    "VASDK/tests/streaming_reply_extractor_test.cpp",
    "VASDK/tests/tool_dispatch_race_test.cpp",
    "VASDK/tests/transcript_score_test.cpp",
    "VASDK/tests/trigger_arbiter_test.cpp",
    "VASDK/tests/turn_assembler_test.cpp",
    "VASDK/tests/turn_policy_test.cpp",
    "VASDK/tests/vad_turn_end_test.cpp",
    "VASDK/tests/wav_file_test.cpp",
    "VASDK_3P_IMPL/EnergyZCRVAD/tests/energy_zcr_vad_test.cpp",
    "VASDK_3P_IMPL/Evaluation/tests/eval_metrics_test.cpp",
    "VASDK_3P_IMPL/GlinerNer/gliner_ner_test.cpp",
    "VASDK_3P_IMPL/LibDataChannelWebRtc/tests/webrtc_provider_test.cpp",
    "VASDK_3P_IMPL/LlamaCpp/llamacpp_completion_test.cpp",
    "VASDK_3P_IMPL/MoonshineSTT/tests/stream_test.cpp",
    "VASDK_3P_IMPL/MossTTS/tests/normalize_test.cpp",
    "VASDK_3P_IMPL/MossTTS/tests/synth_test.cpp",
    "VASDK_3P_IMPL/PhononTTS/tests/chunk_test.cpp",
    "VASDK_3P_IMPL/PhononTTS/tests/normalize_test.cpp",
    "VASDK_3P_IMPL/ProviderCatalog/cloud_key_test.cpp",
    "VASDK_3P_IMPL/ProviderCatalog/dsp_speaker_test.cpp",
    "VASDK_3P_IMPL/ProviderCatalog/provider_options_test.cpp",
    "VASDK_3P_IMPL/ProviderCatalog/tts_prebuffer_test.cpp",
    "VASDK_3P_IMPL/ProviderCatalog/webrtc_session_test.cpp",
    "VASDK_3P_IMPL/PyannoteSpeaker/tests/fbank_test.cpp",
    "VASDK_3P_IMPL/RNNoise/tests/rnnoise_test.cpp",
    "VASDK_3P_IMPL/SmartTurnV3/tests/features_test.cpp",
    "VASDK_3P_IMPL/SupertonicTTS/tests/synth_test.cpp",
    "VASDK_3P_IMPL/Turn1Mini/tests/options_test.cpp",
    "VASDK_3P_IMPL/Turn1Mini/tests/turn1mini_test.cpp",
    "VASDK_3P_IMPL/VizAnim/tests/vizanim_test.cpp",
    "VASDK_3P_IMPL/VozSTT/tests/provider_test.cpp",
    "VASDK_CALLCENTER_APP/crm/callcenter_suite_test.cpp",
    "VASDK_CAR_APP/verticals/climate/climate_test.cpp",
    "VASDK_CAR_APP/verticals/companion/companion_test.cpp",
    "VASDK_CAR_APP/verticals/general/general_test.cpp",
    "VASDK_CAR_APP/verticals/media/media_test.cpp",
    "VASDK_CAR_APP/verticals/messages/messages_test.cpp",
    "VASDK_CAR_APP/verticals/navigation/navigation_test.cpp",
    "VASDK_CAR_APP/verticals/phone/phone_test.cpp",
    "VASDK_CAR_APP/verticals/profiles/profiles_test.cpp",
    "VASDK_PYTHON_APP/tests/test_kitchen.py",
    "VASDK_PYTHON_APP/tests/test_session.py",
    "bindings/python/tests/test_build_trees.py",
    "bindings/python/tests/test_extensions.py",
    "bindings/python/tests/test_garage.py",
    "bindings/python/tests/test_models.py",
    "bindings/python/tests/test_providers.py",
    "bindings/python/tests/test_score.py",
]
MIGRATION_CEILING = 88

LOCAL_HELPER = re.compile(r"\b(?:void|bool|int|auto)\s+(check|expect|checkEq|expectEq|waitUntil|waitFor)\s*\(")
FAILURES_COUNTER = re.compile(r"\bint\s+(?:g_)?failures\b")
MAIN = re.compile(r"\bint\s+main\s*\(")
RETURN_SKIP = re.compile(r"\breturn\s+77\b")
FIXED_TEMPORARY = re.compile(r"temp_directory_path\s*\(\s*\)\s*/\s*\"")
SLEEP = re.compile(r"\bsleep_(?:for|until)\s*\(")
TEST_MACRO = re.compile(r"\bTEST(?:_F|_P)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)")
PASCAL_CASE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
GTEST_KEYWORDS = {"LABEL", "SOURCES", "LIBRARIES", "MODELS_DIR", "ARGS", "TIMEOUT", "RUN_SERIAL", "WORKING_DIRECTORY"}
PYTHON_TEST_FILE = re.compile(r"(?:^|/)test_\w+\.py$")


def strip_comments_and_strings(text):
    """Code only: raw strings, comments and string literals emptied, line numbers kept."""
    text = re.sub(r'R"([^(\s]*)\(.*?\)\1"', lambda m: '""' + "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def cpp_violations(path, text):
    found = []
    code = strip_comments_and_strings(text)
    if MAIN.search(code):
        found.append(f"{path}: has a main(): write GoogleTest cases (TEST, TEST_F, TEST_P); the main is the shared one")
    if not TEST_MACRO.search(code):
        found.append(f"{path}: no TEST, TEST_F or TEST_P case")
    for match in LOCAL_HELPER.finditer(code):
        found.append(f"{path}:{line_of(code, match.start())}: its own {match.group(1)}(): use EXPECT_*/ASSERT_* "
                     f"or va::test::waitUntil (TestSupport.h)")
    for match in FAILURES_COUNTER.finditer(code):
        found.append(f"{path}:{line_of(code, match.start())}: a failures counter: GoogleTest counts")
    for match in RETURN_SKIP.finditer(code):
        found.append(f"{path}:{line_of(code, match.start())}: return 77: GTEST_SKIP() or VA_REQUIRE_FILE")
    for match in FIXED_TEMPORARY.finditer(text):
        found.append(f"{path}:{line_of(text, match.start())}: a temporary folder of a fixed name: "
                     f"va::test::UniqueTemporaryDirectory")
    raw_lines = text.split("\n")
    for number, code_line in enumerate(code.split("\n"), start=1):
        if SLEEP.search(code_line) and "fixed wait:" not in raw_lines[number - 1]:
            found.append(f"{path}:{number}: a sleep without `// fixed wait: <why>`: wait for what you check "
                         f"(va::test::waitUntil)")
    for match in TEST_MACRO.finditer(code):
        for name in match.groups():
            if not PASCAL_CASE.match(name):
                found.append(f"{path}:{line_of(code, match.start())}: {name}: suite and case names in PascalCase")
    return found


def python_violations(path, text):
    found = []
    if not re.search(r"^def test_\w+\s*\(", text, re.MULTILINE):
        found.append(f"{path}: no pytest function (def test_...)")
    if re.search(r"sys\.exit\s*\(\s*main\s*\(", text):
        found.append(f"{path}: sys.exit(main()): pytest runs the test functions")
    if "VASDK_LIBRARY" in text:
        found.append(f"{path}: reads VASDK_LIBRARY: take the library from conftest.py's fixture")
    return found


def calls(text, name):
    """The argument text of every `name(...)` call in a CMake file, with its line."""
    code = re.sub(r"#[^\n]*", "", text)
    # not its definition, function(name ...), nor its name in a message, "name(..."
    for match in re.finditer(r"(?<![\w$(\"])" + re.escape(name) + r"\s*\(", code):
        depth, index = 1, match.end()
        while index < len(code) and depth:
            depth += {"(": 1, ")": -1}.get(code[index], 0)
            index += 1
        yield code[match.end():index - 1], line_of(code, match.start())


def resolve(cmake_path, token):
    """A source named in a CMake file, as a path from the root; None when it isn't a plain path."""
    token = token.strip('"')
    for variable in ("${CMAKE_CURRENT_SOURCE_DIR}/", "${CMAKE_CURRENT_LIST_DIR}/"):
        token = token.replace(variable, "")
    if token.startswith("${VASDK_ROOT_DIR}/"):
        return os.path.normpath(token[len("${VASDK_ROOT_DIR}/"):]).replace(os.sep, "/")
    if "${" in token:
        return None
    return os.path.normpath(os.path.join(os.path.dirname(cmake_path), token)).replace(os.sep, "/")


def cmake_violations(cmake_files, converted):
    found, registered = [], set()
    for path, text in cmake_files.items():
        for arguments, line in calls(text, "vasdk_add_gtest"):
            tokens = arguments.split()
            if "LABEL" not in tokens:
                found.append(f"{path}:{line}: vasdk_add_gtest without a LABEL")
            if "SOURCES" in tokens:
                for token in tokens[tokens.index("SOURCES") + 1:]:
                    if token in GTEST_KEYWORDS:
                        break
                    source = resolve(path, token)
                    if source:
                        registered.add(source)
        for arguments, line in calls(text, "add_executable"):
            for token in arguments.split():
                source = resolve(path, token) if token.strip('"').endswith("_test.cpp") else None
                if source in converted:
                    found.append(f"{path}:{line}: {source} built by add_executable: register it with vasdk_add_gtest")
        for arguments, line in calls(text, "add_test"):
            for token in arguments.split():
                source = resolve(path, token) if PYTHON_TEST_FILE.search(token.strip('"')) else None
                if source in converted:
                    found.append(f"{path}:{line}: {source} run by a bare add_test: register it with vasdk_add_pytest")
    for source in sorted(converted):
        if source.endswith("_test.cpp") and source not in registered:
            found.append(f"{source}: not registered through vasdk_add_gtest")
    return found


def check_tree(sources, cmake_files, not_yet_converted, ceiling):
    """sources: test file path -> text; cmake_files: CMakeLists.txt / *.cmake path -> text."""
    found = []
    if len(not_yet_converted) != ceiling:
        found.append(f"NOT_YET_CONVERTED has {len(not_yet_converted)} files and MIGRATION_CEILING is {ceiling}: "
                     f"the list only shrinks, so lower the ceiling with it, never raise it")
    waiting = set(not_yet_converted)
    for path in sorted(waiting - set(sources)):
        found.append(f"NOT_YET_CONVERTED names {path}, which doesn't exist: take it off")
    converted = set()
    for path, text in sorted(sources.items()):
        file_found = cpp_violations(path, text) if path.endswith(".cpp") else python_violations(path, text)
        if path in waiting:
            if not file_found:
                found.append(f"{path} follows the rules: take it off NOT_YET_CONVERTED")
            continue
        converted.add(path)
        found += file_found
    found += cmake_violations(cmake_files, converted)
    return found


def read_tree(root):
    sources, cmake_files = {}, {}
    for directory, subfolders, files in os.walk(root):
        subfolders[:] = sorted(name for name in subfolders
                               if name not in SKIPPED_FOLDERS and not name.startswith("build"))
        for name in files:
            path = os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/")
            if name.endswith("_test.cpp") or (PYTHON_TEST_FILE.search(path) and "/tests/" in "/" + path):
                target = sources
            elif name == "CMakeLists.txt" or name.endswith(".cmake"):
                target = cmake_files
            else:
                continue
            with open(os.path.join(directory, name), encoding="utf-8", errors="replace") as handle:
                target[path] = handle.read()
    return sources, cmake_files


def self_test():
    good_cpp = ('#include "TestSupport.h"\n'
                'TEST(Thing, DoesWhatItSays) {\n'
                '    EXPECT_TRUE(va::test::waitUntil([] { return true; }));\n'
                '    std::this_thread::sleep_for(10ms);  // fixed wait: checking nothing more comes\n'
                '}\n')
    good_python = "def test_scores(vasdk):\n    assert vasdk.score('a', 'a') == 0\n"
    old_cpp = "int failures = 0;\nvoid check(bool ok, const char* what) {}\nint main() { return failures; }\n"
    good_cmake = "vasdk_add_gtest(vasdk_thing LABEL unit SOURCES tests/thing_test.cpp LIBRARIES VASDK)\n"
    cases = [
        ("a test as the skill writes it", {"VASDK/tests/thing_test.cpp": good_cpp},
         {"VASDK/CMakeLists.txt": good_cmake}, [], []),
        ("a pytest file", {"bindings/python/tests/test_score.py": good_python}, {}, [], []),
        ("its own main and check", {"VASDK/tests/thing_test.cpp": old_cpp},
         {"VASDK/CMakeLists.txt": good_cmake}, [], ["has a main()", "its own check()", "a failures counter",
                                                    "no TEST, TEST_F or TEST_P"]),
        ("a skip by exit code", {"VASDK/tests/thing_test.cpp": good_cpp.replace("}\n", "    return 77;\n}\n", 1)},
         {"VASDK/CMakeLists.txt": good_cmake}, [], ["return 77"]),
        ("a fixed temporary folder", {"VASDK/tests/thing_test.cpp": good_cpp.replace(
            "{\n", '{\n    auto d = std::filesystem::temp_directory_path() / "vasdk_thing";\n', 1)},
         {"VASDK/CMakeLists.txt": good_cmake}, [], ["a temporary folder of a fixed name"]),
        ("a bare sleep", {"VASDK/tests/thing_test.cpp": good_cpp.replace("  // fixed wait: checking nothing more comes", "")},
         {"VASDK/CMakeLists.txt": good_cmake}, [], ["a sleep without"]),
        ("a snake_case name", {"VASDK/tests/thing_test.cpp": good_cpp.replace("DoesWhatItSays", "does_what_it_says")},
         {"VASDK/CMakeLists.txt": good_cmake}, [], ["suite and case names in PascalCase"]),
        ("no LABEL", {"VASDK/tests/thing_test.cpp": good_cpp},
         {"VASDK/CMakeLists.txt": good_cmake.replace("LABEL unit ", "")}, [], ["without a LABEL"]),
        ("a bare add_executable", {"VASDK/tests/thing_test.cpp": good_cpp},
         {"VASDK/CMakeLists.txt": "add_executable(vasdk_thing_test tests/thing_test.cpp)\n"
                                  "add_test(NAME vasdk_thing COMMAND vasdk_thing_test)\n"},
         [], ["built by add_executable", "not registered through vasdk_add_gtest"]),
        ("a bare add_test for pytest", {"bindings/python/tests/test_score.py": good_python},
         {"CMakeLists.txt": "add_test(NAME vasdk_python_score COMMAND python "
                            "${VASDK_ROOT_DIR}/bindings/python/tests/test_score.py)\n"}, [], ["run by a bare add_test"]),
        ("a script, not pytest", {"bindings/python/tests/test_score.py":
                                  "import os, sys\nlibrary = os.environ['VASDK_LIBRARY']\n"
                                  "def main():\n    return 0\nsys.exit(main())\n"},
         {}, [], ["no pytest function", "sys.exit(main())", "reads VASDK_LIBRARY"]),
        ("an old-style file waiting its turn", {"VASDK/tests/old_test.cpp": old_cpp},
         {"VASDK/CMakeLists.txt": "add_executable(vasdk_old_test tests/old_test.cpp)\n"},
         ["VASDK/tests/old_test.cpp"], []),
        ("a converted file left on the list", {"VASDK/tests/thing_test.cpp": good_cpp},
         {"VASDK/CMakeLists.txt": good_cmake}, ["VASDK/tests/thing_test.cpp"], ["take it off NOT_YET_CONVERTED"]),
        ("a file added to the list", {"VASDK/tests/old_test.cpp": old_cpp, "VASDK/tests/new_test.cpp": old_cpp},
         {}, ["VASDK/tests/old_test.cpp", "VASDK/tests/new_test.cpp"], ["the list only shrinks"]),
        ("a listed file that's gone", {}, {}, ["VASDK/tests/gone_test.cpp"], ["which doesn't exist"]),
    ]
    failures = 0
    for name, sources, cmake_files, waiting, expected in cases:
        ceiling = 1 if name == "a file added to the list" else len(waiting)
        found = check_tree(sources, cmake_files, waiting, ceiling)
        ok = all(any(e in f for f in found) for e in expected) and (bool(found) == bool(expected))
        if not ok:
            failures += 1
            print(f"FAIL {name}: expected {expected}, found {found}")
    if failures:
        return 1
    print(f"self-test: {len(cases)} cases ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    sources, cmake_files = read_tree(arguments.root)
    found = check_tree(sources, cmake_files, NOT_YET_CONVERTED, MIGRATION_CEILING)
    for line in found:
        print(line)
    if found:
        print(f"{len(found)} test rule violation(s); see skills/vasdk-testing/SKILL.md")
        return 1
    print(f"test rules ok ({len(sources) - len(NOT_YET_CONVERTED)} tests follow them, "
          f"{len(NOT_YET_CONVERTED)} still to convert)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
