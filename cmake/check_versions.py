#!/usr/bin/env python3
"""Checks that every place stating VASDK's version agrees with the VERSION file (docs/releasing.md).

  python3 cmake/check_versions.py            check this repository
  python3 cmake/check_versions.py --tag v1.2.3   also check a release tag against it

The CMake projects, the Kotlin binding and the Android build read VERSION themselves; the Python
binding's pyproject.toml states it (release-please updates it with VERSION). Standard library only.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main(arguments):
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    problems = []
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        problems.append(f"VERSION is '{version}': MAJOR.MINOR.PATCH")
    pyproject = (ROOT / "bindings" / "python" / "pyproject.toml").read_text(encoding="utf-8")
    stated = re.search(r'^version\s*=\s*"([^"]+)"', pyproject, re.MULTILINE)
    if not stated or stated.group(1) != version:
        problems.append(f"bindings/python/pyproject.toml says {stated.group(1) if stated else 'nothing'}, VERSION says {version}")
    for path, needle in [("CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("VASDK/CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("VASDK_ANDROID/CMakeLists.txt", "VERSION ${VASDK_VERSION}"),
                         ("bindings/kotlin/build.gradle.kts", '"../../VERSION"')]:
        if needle not in (ROOT / path).read_text(encoding="utf-8"):
            problems.append(f"{path} doesn't read the VERSION file")
    if "--tag" in arguments:
        tag = arguments[arguments.index("--tag") + 1]
        if tag != f"v{version}":
            problems.append(f"the tag is {tag}, VERSION says {version}: tag v{version}")
    for problem in problems:
        print(problem)
    if problems:
        return 1
    print(f"versions ok: {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
