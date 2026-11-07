#!/usr/bin/env python3
"""Checks the third-party code rules (AGENTS.md, "Third-party integrations") on VASDK_3P_IMPL.

  python3 VASDK_3P_IMPL/check_rules.py              check every integration
  python3 VASDK_3P_IMPL/check_rules.py --self-test  check the checker on planted cases

Rules checked:
  1. every header and source of an integration declares its code in namespace
     va::thirdparty::<category>, the category being one of CATEGORIES (the catalog, which
     registers every category, sits in va::thirdparty itself); inside it, the core namespaces
     that a category shadows (audio, transport, llm, nlu) are written in full: va::audio::
  2. no `static` variable in a source file (.cpp/.cc), at file scope or in a function; static
     functions and class constants are fine, fixed tables are constexpr
Tests (tests/ folders, *_test.cpp), programs (a main()) and the exemptions below are skipped.
Standard library only.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SOURCE_EXTENSIONS = (".h", ".hpp", ".cpp", ".cc")
SKIPPED_FOLDERS = {"tests", "test", "cmake", "golden", "golden-gen", "third_party", "proto"}

NAMESPACE_EXEMPT = {
    "MiniAudio/miniaudio_impl.cpp": "compiles the miniaudio library itself",
    "ProviderCatalog/ProviderCatalogC.h": "the C API (extern \"C\")",
    "ProviderCatalog/ProviderCatalogC.cpp": "the C API (extern \"C\")",
}
STATIC_EXEMPT = {
    "WhistleSTT/impl/NeedleEngine.cpp": "the needle engine is process-global (one model, no unload), "
                                        "so its wrapper and mutex are too",
}

# One per stage skill (skills/vasdk-<stage>); a new kind of integration adds its category here and in AGENTS.md.
CATEGORIES = ("vad", "stt", "tts", "turn", "wakeword", "llm", "nlu", "speaker", "audio", "transport")
CATALOG_FOLDER = "ProviderCatalog/"
NAMESPACE = re.compile(r"namespace\s+(?:va::thirdparty|va\s*\{\s*namespace\s+thirdparty)(?:::(\w+)(?:::\w+)*)?\s*\{")
SHADOWED_CORE = re.compile(r"(?<![\w:])(audio|transport|llm|nlu)::(?=[A-Za-z_])")
STATIC_LINE = re.compile(r"^\s*(?:inline\s+)?static\b(?!_)(.*)$", re.MULTILINE)


def strip_comments_and_strings(text):
    text = re.sub(r'R"([^(\s]*)\(.*?\)\1"', '""', text, flags=re.DOTALL)
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


def is_variable(declaration):
    """`static int x = 1;` is a variable; `static int f(int)` is a function."""
    for character in declaration:
        if character == "(":
            return False
        if character in "=;{[":
            return True
    return True  # a declaration running onto the next line without a parenthesis yet


def violations(relative_path, text):
    found = []
    code = strip_comments_and_strings(text)
    if relative_path not in NAMESPACE_EXEMPT:
        categories = [match.group(1) for match in NAMESPACE.finditer(code)]
        if relative_path.startswith(CATALOG_FOLDER):
            if not categories:
                found.append(f"{relative_path}: not in namespace va::thirdparty")
        elif not any(category in CATEGORIES for category in categories):
            found.append(f"{relative_path}: not in namespace va::thirdparty::<category> "
                         f"(one of {', '.join(CATEGORIES)})")
        # in the catalog (va::thirdparty), nlu:: rightly names the category, so only categories are checked
        for match in ([] if relative_path.startswith(CATALOG_FOLDER) else SHADOWED_CORE.finditer(code)):
            line = code.count("\n", 0, match.start()) + 1
            found.append(f"{relative_path}:{line}: write va::{match.group(1)}:: in full "
                         f"(va::thirdparty::{match.group(1)} shadows it)")
    if relative_path.endswith((".cpp", ".cc")) and relative_path not in STATIC_EXEMPT:
        for match in STATIC_LINE.finditer(code):
            if is_variable(match.group(1)):
                line = code.count("\n", 0, match.start()) + 1
                found.append(f"{relative_path}:{line}: static variable: {match.group(0).strip()[:90]}")
    return found


def integration_files(root):
    for folder in sorted(os.listdir(root)):
        if not os.path.isfile(os.path.join(root, folder, "CMakeLists.txt")):
            continue
        for directory, subfolders, files in os.walk(os.path.join(root, folder)):
            subfolders[:] = sorted(name for name in subfolders if name not in SKIPPED_FOLDERS)
            for name in sorted(files):
                if name.endswith(SOURCE_EXTENSIONS) and not name.endswith("_test.cpp"):
                    path = os.path.join(directory, name)
                    yield os.path.relpath(path, root).replace(os.sep, "/"), path


def check(root):
    found = []
    for relative_path, path in integration_files(root):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        if re.search(r"\bint\s+main\s*\(", text):
            continue
        found += violations(relative_path, text)
    return found


def self_test():
    good = 'namespace va::thirdparty::stt {\nnamespace {\nconstexpr int kRate = 16000;\nint helper(int x) { return x; }\n}\n'
    cases = [
        ("Good/Good.cpp", good + 'static int twice(int x) { return 2 * x; }\nstruct A { static constexpr int k = 1; static A& get(); };\n}\n', []),
        ("Old/Old.h", "class Old {};\n", ["not in namespace va::thirdparty::<category>"]),
        ("Flat/Flat.h", "namespace va::thirdparty {\nclass F {};\n}\n", ["not in namespace va::thirdparty::<category>"]),
        ("Unknown/Unknown.h", "namespace va::thirdparty::misc {\nclass U {};\n}\n", ["not in namespace va::thirdparty::<category>"]),
        ("Nested/Nested.h", "namespace va {\nnamespace thirdparty::tts {\nclass N {};\n}\n}\n", []),
        ("Port/impl/port.h", "namespace va::thirdparty::tts::port {\nclass P {};\n}\n", []),
        ("ProviderCatalog/Catalog.h", "namespace va::thirdparty {\nvoid registerAll();\nusing E = nlu::Encoder;\n}\n", []),
        ("Shadow/Shadow.h", "namespace va::thirdparty::stt {\nusing Config = audio::AudioConfig;\n"
                            "using Full = va::audio::AudioConfig;\n}\n", ["write va::audio:: in full"]),
        ("File/File.cpp", good + "static int counter = 0;\n}\n", ["static variable: static int counter = 0;"]),
        ("Local/Local.cpp", good + "void f() {\n    static std::mutex lock;\n}\n}\n", ["static variable: static std::mutex lock;"]),
        ("Table/Table.cpp", good + 'void f() {\n    static const char* const kNames[] = {"a"};\n}\n}\n', ["static variable: static const char* const kNames[] ="]),
        ("Comment/Comment.cpp", good + "// static int notCode = 0;\n/* static int alsoNot = 1; */\n}\n", []),
        ("Header/Header.h", "namespace va::thirdparty::vad {\nstatic int allowedInHeaders = 0;\n}\n", []),
        ("WhistleSTT/impl/NeedleEngine.cpp", good + "Engine& get() { static Engine engine; return engine; }\n}\n", []),
    ]
    failures = 0
    for path, text, expected in cases:
        found = violations(path, text)
        ok = len(found) == len(expected) and all(any(e in f for f in found) for e in expected)
        if not ok:
            failures += 1
            print(f"FAIL {path}: expected {expected}, found {found}")
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
    found = check(arguments.root)
    for line in found:
        print(line)
    if found:
        print(f"{len(found)} third-party rule violation(s); see AGENTS.md, \"Third-party integrations\"")
        return 1
    print("third-party rules ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
