#!/usr/bin/env python3
"""Checks that every place stating VASDK's version agrees with the VERSION file (docs/releasing.md).

  python3 cmake/check_versions.py               check this repository
  python3 cmake/check_versions.py --set 1.2.3   write 1.2.3 everywhere it's stated, then check

The CMake projects, the Kotlin binding, the Android build and conanfile.py read VERSION themselves;
the Python binding's pyproject.toml states it (--set writes both; a release does it with the version
cmake/next_version.py gives). Also checks
that conanfile.py requires the dependency versions the CMake build fetches. Standard library only.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The Conan package against the versions the CMake build fetches: (Conan package, the file that
# pins it, a pattern for its version there, the prefix Conan adds: protobuf 36.2 is Conan's 7.36.2).
CONAN_DEPENDENCIES = [
    ("spdlog", "CMakeLists.txt", r'spdlog\.git\s+GIT_TAG\s+"v([\d.]+)"', ""),
    ("nlohmann_json", "CMakeLists.txt", r"json\.git\s+GIT_TAG\s+v([\d.]+)", ""),
    ("highway", "CMakeLists.txt", r'highway\.git\s+GIT_TAG\s+"([\d.]+)"', ""),
    ("cpp-httplib", "VASDK/CMakeLists.txt", r"cpp-httplib\.git\s+GIT_TAG\s+v([\d.]+)", ""),
    ("protobuf", "VASDK/cmake/VASDKProtobuf.cmake", r"VASDK_PROTOBUF_VERSION\s+([\d.]+)\)", "7."),
    ("libcurl", "VASDK/cmake/VASDKCurl.cmake", r"curl-(\d+)_(\d+)_(\d+)\.tar\.gz", ""),
]


def conan_dependency_problems():
    recipe = (ROOT / "conanfile.py").read_text(encoding="utf-8")
    problems = []
    for name, path, pattern, conan_prefix in CONAN_DEPENDENCIES:
        pinned = re.search(pattern, (ROOT / path).read_text(encoding="utf-8"))
        required = re.search(rf'self\.requires\("{re.escape(name)}/([^"]+)"', recipe)
        if not pinned or not required:
            problems.append(f"conanfile.py: can't find {name}'s version in {path if not pinned else 'conanfile.py'}")
            continue
        expected = conan_prefix + ".".join(pinned.groups())
        if required.group(1) != expected:
            problems.append(f"conanfile.py requires {name}/{required.group(1)}, {path} fetches {expected}")
    return problems


PYPROJECT_VERSION = re.compile(r'^version\s*=\s*"([^"]+)"', re.MULTILINE)


def set_version(version):
    """Writes `version` into VERSION and the Python binding's pyproject.toml."""
    (ROOT / "VERSION").write_text(version + "\n", encoding="utf-8")
    pyproject_path = ROOT / "bindings" / "python" / "pyproject.toml"
    pyproject = pyproject_path.read_text(encoding="utf-8")
    pyproject_path.write_text(PYPROJECT_VERSION.sub(f'version = "{version}"', pyproject, count=1),
                              encoding="utf-8")


def main(arguments):
    if "--set" in arguments:
        set_version(arguments[arguments.index("--set") + 1])
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    problems = []
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        problems.append(f"VERSION is '{version}': MAJOR.MINOR.PATCH")
    pyproject = (ROOT / "bindings" / "python" / "pyproject.toml").read_text(encoding="utf-8")
    stated = PYPROJECT_VERSION.search(pyproject)
    if not stated or stated.group(1) != version:
        problems.append(f"bindings/python/pyproject.toml says {stated.group(1) if stated else 'nothing'}, VERSION says {version}")
    for path, needle in [("CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("VASDK/CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("VASDK_ANDROID/CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("bindings/kotlin/build.gradle.kts", '"../../VERSION"'),
                         ("conanfile.py", '"VERSION"')]:
        if needle not in (ROOT / path).read_text(encoding="utf-8"):
            problems.append(f"{path} doesn't read the VERSION file")
    problems += conan_dependency_problems()
    for problem in problems:
        print(problem)
    if problems:
        return 1
    print(f"versions ok: {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
