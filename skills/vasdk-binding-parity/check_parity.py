#!/usr/bin/env python3
"""Checks that C, Python and Kotlin keep up with the C++ API (skills/vasdk-binding-parity).

  python3 skills/vasdk-binding-parity/check_parity.py                       every binding
  python3 skills/vasdk-binding-parity/check_parity.py --bindings c,python   leave Kotlin out
  python3 skills/vasdk-binding-parity/check_parity.py --root DIR            another tree laid out the same way
  python3 skills/vasdk-binding-parity/check_parity.py --self-test           every fault planted in the fixture is found

The manifest, parity.md beside this script, lists the audited C++ classes and one row per
capability: the C function, Python and Kotlin names that expose it, or "—". Reported:
  - a public method of an audited class (or exported function of an audited header) with no row
  - a row whose C++ symbol is gone, or whose C / Python / Kotlin name doesn't exist
  - a row with "—" for a binding, unless its note starts "C++ only:" (then every binding is "—")
  - a C function a binding doesn't declare, unless "C functions the bindings leave out" lists it
  - a C function a binding declares that the C API no longer has
Standard library only. The C++ scan is a line scanner, not a parser: it reads declarations at
class level in public sections, which is how VASDK's headers are written.
"""

import argparse
import glob
import os
import re
import sys

MANIFEST = os.path.join("skills", "vasdk-binding-parity", "parity.md")
C_HEADERS = [os.path.join("VASDK", "include", "VASDK", "c", "*.h"),
             os.path.join("VASDK_3P_IMPL", "ProviderCatalog", "ProviderCatalogC.h")]
PYTHON = os.path.join("bindings", "python", "vasdk", "__init__.py")
KOTLIN = os.path.join("bindings", "kotlin", "src", "main", "kotlin")
BINDINGS = ("c", "python", "kotlin")
NONE = "—"
CPP_ONLY = "C++ only:"

KEYWORDS = {"void", "int", "bool", "float", "double", "char", "auto", "const", "return", "if", "for", "while",
            "switch", "sizeof", "decltype", "static_cast", "operator", "explicit", "virtual", "static", "inline",
            "override"}
CLASS = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(class|struct)\s+(?:VASDK_EXPORT\s+)?(\w+)\b[^;]*$")
ACCESS = re.compile(r"^\s*(public|private|protected)\s*:")
METHOD = re.compile(r"^\s*(?:\[\[\w+\]\]\s*)?(?:(?:virtual|static|explicit|inline|friend)\s+)*"
                    r"[\w:<>,\s\*&]*?\b([a-z]\w*)\s*\(")
EXPORTED_FUNCTION = re.compile(r"^\s*VASDK_EXPORT\s+[\w:<>,\s\*&]*?\b([a-z]\w*)\s*\(", re.MULTILINE)
C_FUNCTION = re.compile(r"^(?!\s*typedef)[ \t]*(?:VASDK_EXPORT\s+)?[A-Za-z_][\w \t\*]*?\b(vasdk_[a-z0-9_]+)\s*\(",
                        re.MULTILINE)
PYTHON_DECLARED = re.compile(r"""_declare\(\s*"(vasdk_[a-z0-9_]+)"|\.(vasdk_[a-z0-9_]+)\b""")
KOTLIN_DECLARED = re.compile(r"\bfun\s+(vasdk_[a-z0-9_]+)\s*\(")
SYMBOL = re.compile(r"`([^`]+)`")
SECTION = re.compile(r"^## (.+)$", re.MULTILINE)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def public_methods(text, wanted):
    """{class: [method, ...]} for the classes in `wanted`: their public declarations at class level."""
    found = {name: [] for name in wanted}
    stack, depth, pending = [], 0, None   # stack: [class, body depth, current access]
    for line in strip_comments(text).splitlines():
        opened = CLASS.match(line)
        if opened:
            pending = (opened.group(2), "public" if opened.group(1) == "struct" else "private")
        frame = stack[-1] if stack else None
        if frame and depth == frame[1]:
            access = ACCESS.match(line)
            if access:
                frame[2] = access.group(1)
            elif frame[2] == "public" and frame[0] in wanted:
                method = METHOD.match(line)
                if method and method.group(1) not in KEYWORDS and "~" not in line.split("(")[0]:
                    found[frame[0]].append(method.group(1))
        for character in line:
            if character == "{":
                depth += 1
                if pending:
                    stack.append([pending[0], depth, pending[1]])
                    pending = None
            elif character == "}":
                if stack and stack[-1][1] == depth:
                    stack.pop()
                depth -= 1
        if pending and line.rstrip().endswith(";"):
            pending = None
    return found


def tables(markdown):
    """{section title: [[cell, ...], ...]}: the rows of each section's table, header and rule dropped."""
    result = {}
    parts = SECTION.split(markdown)
    for title, body in zip(parts[1::2], parts[2::2]):
        rows = [line.strip().strip("|").split("|") for line in body.splitlines() if line.strip().startswith("|")]
        result[title.strip()] = [[cell.strip() for cell in row] for row in rows[2:]]
    return result


def symbols(cell):
    return [] if cell.strip() in ("", NONE) else SYMBOL.findall(cell)


def c_functions(root):
    names = set()
    for pattern in C_HEADERS:
        for path in glob.glob(os.path.join(root, pattern)):
            names.update(C_FUNCTION.findall(strip_comments(read(path))))
    return names


def kotlin_sources(root):
    return [os.path.join(folder, name) for folder, _, files in os.walk(os.path.join(root, KOTLIN))
            for name in files if name.endswith(".kt")]


def python_api(text):
    """{class or "": {member, ...}}: the module's functions under "", each class's methods under its name."""
    api = {"": set()}
    current = ""
    for line in text.splitlines():
        top_class = re.match(r"^class\s+(\w+)", line)
        top_def = re.match(r"^def\s+(\w+)", line)
        member = re.match(r"^\s+def\s+(\w+)", line)
        if top_class:
            current = top_class.group(1)
            api.setdefault(current, set())
            api[""].add(current)
        elif top_def:
            current = ""
            api[""].add(top_def.group(1))
        elif member and current:
            api[current].add(member.group(1))
    return api


def python_has(api, name):
    owner, _, member = name.rpartition(".")
    return member in api.get(owner, set())


def kotlin_has(text, name):
    # shortcut: owner and member are looked for anywhere in the sources, not nested; enough to catch
    # a rename or a missing method, not a method moved to another class
    owner, _, member = name.rpartition(".")
    if owner and not re.search(rf"\b(class|object|interface)\s+{re.escape(owner.split('.')[-1])}\b", text):
        return False
    return re.search(rf"\b(fun|val|var|class|object|interface)\s+(<[^>]*>\s*)?{re.escape(member)}\b", text) is not None


def check(root, bindings):
    errors = []
    manifest_path = os.path.join(root, MANIFEST)
    if not os.path.isfile(manifest_path):
        return [f"{manifest_path}: not found"]
    sections = tables(read(manifest_path))
    audited = sections.get("Audited C++", [])
    rows = sections.get("Capabilities", [])
    left_out = sections.get("C functions the bindings leave out", [])
    if not audited or not rows:
        return [f"{manifest_path}: needs the sections 'Audited C++' and 'Capabilities', each with a table"]

    # Every audited C++ symbol, as the rows name them: Class::method, Class, or function
    cpp = set()
    for cells in audited:
        header, owner, audit = (symbols(cell)[0] if symbols(cell) else cell for cell in cells[:3])
        path = os.path.join(root, header)
        if not os.path.isfile(path):
            errors.append(f"{manifest_path}: audited header doesn't exist: {header}")
            continue
        text = read(path)
        if audit == "functions":
            cpp.update(EXPORTED_FUNCTION.findall(strip_comments(text)))
        elif audit == "methods":
            cpp.update(f"{owner}::{method}" for method in public_methods(text, {owner})[owner])
            if not re.search(rf"\b(class|struct)\s+(VASDK_EXPORT\s+)?{owner}\b", text):
                errors.append(f"{manifest_path}: class {owner} isn't in {header}")
        elif audit == "class":
            cpp.add(owner)
            if not re.search(rf"\b(class|struct)\s+(VASDK_EXPORT\s+)?{owner}\b", text):
                errors.append(f"{manifest_path}: class {owner} isn't in {header}")
        else:
            errors.append(f"{manifest_path}: audit '{audit}' for {owner}: methods, class or functions")

    c_names = c_functions(root)
    python_text = read(os.path.join(root, PYTHON)) if os.path.isfile(os.path.join(root, PYTHON)) else ""
    python = python_api(python_text)
    kotlin_text = "\n".join(read(path) for path in kotlin_sources(root))
    exists = {"c": lambda name: name in c_names,
              "python": lambda name: python_has(python, name),
              "kotlin": lambda name: kotlin_has(kotlin_text, name)}

    covered = set()
    for cells in rows:
        if len(cells) < 5:
            errors.append(f"{manifest_path}: row needs 5 cells (C++ | C | Python | Kotlin | Note): {' | '.join(cells)}")
            continue
        names = symbols(cells[0])
        note = cells[4]
        cpp_only = note.startswith(CPP_ONLY)
        for name in names:
            covered.add(name)
            if name not in cpp:
                errors.append(f"{manifest_path}: {name} isn't public in the audited C++ (gone or renamed?)")
        label = ", ".join(names) or cells[0]
        for column, binding in zip(cells[1:4], BINDINGS):
            named = symbols(column)
            if cpp_only:
                if named:
                    errors.append(f"{manifest_path}: {label} is '{CPP_ONLY}' but names a {binding} symbol")
                continue
            if binding not in bindings:
                continue
            if not named:
                errors.append(f"{label}: no {binding} binding (bind it, or note '{CPP_ONLY} <why>')")
            for name in named:
                if not exists[binding](name):
                    errors.append(f"{manifest_path}: {label}: {binding} has no {name}")
    for name in sorted(cpp - covered):
        errors.append(f"{name}: public C++ with no row in {MANIFEST}")

    # The C API, function by function, against what each binding declares
    left_out_names = set()
    for cells in left_out:
        for name in symbols(cells[0]):
            left_out_names.add(name)
            if name not in c_names:
                errors.append(f"{manifest_path}: left-out C function doesn't exist: {name}")
    declared = {}
    if "python" in bindings:
        declared["python"] = {a or b for a, b in PYTHON_DECLARED.findall(python_text)}
    if "kotlin" in bindings:
        declared["kotlin"] = set(KOTLIN_DECLARED.findall(kotlin_text))
    for binding, names in declared.items():
        for name in sorted(c_names - names - left_out_names):
            errors.append(f"{name}: C function {binding} doesn't declare")
        for name in sorted(names - c_names):
            errors.append(f"{name}: {binding} declares it, the C API doesn't have it")
    return errors


# The faults planted in tests/fixture; each must be reported, and nothing else.
EXPECTED_FAULTS = [
    "Widget::spin: public C++ with no row",
    "Widget::gone isn't public in the audited C++",
    "Widget::stop: no kotlin binding",
    "Widget::start: python has no Widget.begin",
    "vasdk_widget_spin: C function python doesn't declare",
    "vasdk_widget_spin: C function kotlin doesn't declare",
    "vasdk_widget_old: python declares it, the C API doesn't have it",
    "left-out C function doesn't exist: vasdk_widget_missing",
    "Widget::internals is 'C++ only:' but names a c symbol",
]


def self_test():
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixture")
    errors = check(fixture, BINDINGS)
    missed = [fault for fault in EXPECTED_FAULTS if not any(fault in error for error in errors)]
    unexpected = [error for error in errors if not any(fault in error for fault in EXPECTED_FAULTS)]
    for fault in missed:
        print(f"MISSED: {fault}")
    for error in unexpected:
        print(f"UNEXPECTED: {error}")
    assert public_methods("class A {\n  int hidden();\npublic:\n  void run(int x) { if (x) go(); }\n"
                          "  struct Options { void inner(); };\n  virtual ~A();\n};\n", {"A"}) == {"A": ["run"]}
    if missed or unexpected:
        return 1
    print(f"self-test: all {len(EXPECTED_FAULTS)} planted faults found, nothing else")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    parser.add_argument("--bindings", default=",".join(BINDINGS), help="which to check: c,python,kotlin")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    bindings = [name.strip() for name in arguments.bindings.split(",") if name.strip()]
    unknown = [name for name in bindings if name not in BINDINGS]
    if unknown:
        parser.error(f"unknown binding(s): {', '.join(unknown)}")
    errors = check(os.path.abspath(arguments.root), bindings)
    for error in errors:
        print(error)
    if errors:
        print(f"{len(errors)} parity gap(s): see skills/vasdk-binding-parity/SKILL.md")
        return 1
    print(f"bindings at parity ({', '.join(bindings)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
