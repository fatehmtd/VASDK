"""Copies a shared build of VASDK, and the libraries beside it that it needs, into the vasdk
package, so the wheel built next carries them:

    python bindings/python/bundle.py build/VASDK/Release      # where VASDK.dll / libVASDK.so is
    pip wheel bindings/python -w dist

On Windows every DLL in that folder is copied (protobuf, abseil, curl, OpenSSL, zlib sit there);
elsewhere libVASDK and the .so / .dylib files beside it. `--clean` removes what was copied.
"""

import shutil
import sys
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent / "vasdk"
PATTERNS = {"win32": ["*.dll"], "darwin": ["*.dylib"]}.get(sys.platform, ["*.so", "*.so.*"])
LIBRARY = {"win32": "VASDK.dll", "darwin": "libVASDK.dylib"}.get(sys.platform, "libVASDK.so")


def clean():
    for pattern in PATTERNS:
        for found in PACKAGE.glob(pattern):
            found.unlink()


def main(arguments):
    if arguments[:1] == ["--clean"]:
        clean()
        return 0
    if len(arguments) != 1:
        print(__doc__)
        return 2
    folder = Path(arguments[0]).resolve()
    if not (folder / LIBRARY).exists():
        print(f"{LIBRARY} isn't in {folder}: build VASDK as a shared library (the default on Windows)")
        return 1
    clean()
    copied = []
    for pattern in PATTERNS:
        for found in sorted(folder.glob(pattern)):
            if found.is_file():
                shutil.copy2(found, PACKAGE / found.name)
                copied.append(found.name)
    print(f"copied into {PACKAGE}: {', '.join(copied)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
