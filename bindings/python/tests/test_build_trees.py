"""Where the binding looks for VASDK's build: build/ and its preset folders (build/dev after cmake
--preset dev), the most recently built library first, VASDK_BUILD_DIR when set, and the messages
and providers from the library's own build tree. Fake build trees in a temporary folder.

    VASDK_LIBRARY=build/cmake/c_library/libvasdk_c.so python3 bindings/python/tests/test_build_trees.py
"""

import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402

LIBRARY = vasdk._C_LIBRARY_NAMES[0]   # vasdk_c.dll, libvasdk_c.so, libvasdk_c.dylib
SHARED = vasdk._C_LIBRARY_NAMES[-1]   # a shared VASDK: VASDK.dll, libVASDK.so, ...


def tree(base, name=None):
    """A build tree: a folder with a CMakeCache.txt."""
    folder = base / name if name else base
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "CMakeCache.txt").write_text("", encoding="utf-8")
    return folder


def library(folder, name, modified):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / name).write_bytes(b"")
    os.utime(folder / name, (modified, modified))
    return folder / name


def main():
    saved = os.environ.pop("VASDK_BUILD_DIR", None)
    try:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            top = tree(build)                    # cmake -B build
            dev = tree(build, "dev")             # cmake --preset dev
            release = tree(build, "release")     # cmake --preset release
            (build / "_deps").mkdir()            # a folder without a CMakeCache.txt: not a build tree

            trees = vasdk._build_trees([build])
            assert [found.name for found in trees] == ["build", "dev", "release"], trees

            assert vasdk._newest_library(trees) is None, "no library built yet"

            # Only the preset build has the library: found there (the bug: only build/ was searched)
            built_dev = library(dev / "cmake" / "c_library" / "Release", LIBRARY, 1_000_000)
            assert vasdk._newest_library(trees) == (str(built_dev), dev)

            # Built in two trees: the most recent wins
            built_top = library(top / "cmake" / "c_library", LIBRARY, 2_000_000)
            assert vasdk._newest_library(trees) == (str(built_top), top)
            os.utime(built_dev, (3_000_000, 3_000_000))
            assert vasdk._newest_library(trees) == (str(built_dev), dev)

            # vasdk_c wins over a shared VASDK, even a newer one
            if SHARED != LIBRARY:
                library(release / "VASDK", SHARED, 4_000_000)
                assert vasdk._newest_library(trees) == (str(built_dev), dev)

            # The same tree under two bases (./build is the source tree's build) is listed once
            assert len(vasdk._build_trees([build, Path(directory) / "build"])) == 3

            # VASDK_BUILD_DIR: that tree only
            os.environ["VASDK_BUILD_DIR"] = str(release)
            assert vasdk._build_trees([build]) == [release]
            del os.environ["VASDK_BUILD_DIR"]
    finally:
        if saved is not None:
            os.environ["VASDK_BUILD_DIR"] = saved

    # The library this run loaded: from VASDK_LIBRARY (ctest sets it) there's no build tree to name
    found = vasdk.build_dir()
    assert found is None or (found / "CMakeCache.txt").exists(), found
    if os.environ.get("VASDK_LIBRARY"):
        assert found is None, found
    print("build trees ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
