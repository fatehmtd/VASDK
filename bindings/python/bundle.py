"""Copies a shared build of VASDK, and the libraries beside it that it needs, into the vasdk
package, so the wheel built next carries them:

    python bindings/python/bundle.py build/VASDK/Release      # where VASDK.dll / libVASDK.so is
    pip wheel bindings/python -w dist

On Windows every DLL in that folder is copied (protobuf, abseil, curl, OpenSSL, zlib sit there);
elsewhere libVASDK and the .so / .dylib files beside it. `--clean` removes what was copied.

For spoken sessions (vasdk.Providers), name the speech providers' folder too: its vasdk_providers
library and what it loads (ONNX Runtime) are copied as well. The models aren't: Session's models_dir.

    python bindings/python/bundle.py build/VASDK/Release build/VASDK_3P_IMPL/ProviderCatalog/Release

VASDK's Python messages (vasdk.engine, vasdk.flow, ...), which the build generates into
build/bindings/python/vasdk, are copied in too: found above the library's folder, or from
VASDK_PYTHON_PROTOS.
"""

import os
import shutil
import sys
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent / "vasdk"
PATTERNS = {"win32": ["*.dll"], "darwin": ["*.dylib"]}.get(sys.platform, ["*.so", "*.so.*"])
LIBRARY = {"win32": "VASDK.dll", "darwin": "libVASDK.dylib"}.get(sys.platform, "libVASDK.so")
PROVIDERS = {"win32": "vasdk_providers.dll", "darwin": "libvasdk_providers.dylib"}.get(sys.platform, "libvasdk_providers.so")
MESSAGES = ["engine", "flow", "goal", "nlu", "providers"]   # the proto packages, as vasdk.<name>


def clean():
    for pattern in PATTERNS:
        for found in PACKAGE.glob(pattern):
            found.unlink()
    for name in MESSAGES:
        shutil.rmtree(PACKAGE / name, ignore_errors=True)


def find_messages(library_folder):
    """The folder holding engine/events_pb2.py: VASDK_PYTHON_PROTOS, or build/bindings/python/vasdk above the library."""
    if os.environ.get("VASDK_PYTHON_PROTOS"):
        return Path(os.environ["VASDK_PYTHON_PROTOS"]).resolve()
    for folder in [library_folder, *library_folder.parents][:4]:
        candidate = folder / "bindings" / "python" / "vasdk"
        if (candidate / "engine" / "events_pb2.py").exists():
            return candidate
    return None


def main(arguments):
    if arguments[:1] == ["--clean"]:
        clean()
        return 0
    if len(arguments) not in (1, 2):
        print(__doc__)
        return 2
    folders = [Path(argument).resolve() for argument in arguments]
    if not (folders[0] / LIBRARY).exists():
        print(f"{LIBRARY} isn't in {folders[0]}: build VASDK as a shared library (the default on Windows)")
        return 1
    if len(folders) > 1 and not any(folders[1].glob(PROVIDERS)):
        print(f"no vasdk_providers library in {folders[1]}: build the vasdk_providers target")
        return 1
    messages = find_messages(folders[0])
    if messages is None:
        print("VASDK's Python messages aren't in the build (build/bindings/python/vasdk): build the vasdk_python_protos "
              "target, or set VASDK_PYTHON_PROTOS")
        return 1
    clean()
    for name in MESSAGES:
        if (messages / name).is_dir():
            shutil.copytree(messages / name, PACKAGE / name, ignore=shutil.ignore_patterns("__pycache__"))
    copied = []
    for folder in folders:
        for pattern in PATTERNS:
            for found in sorted(folder.glob(pattern)):
                if found.is_file() and found.name not in copied:
                    shutil.copy2(found, PACKAGE / found.name)
                    copied.append(found.name)
    print(f"copied into {PACKAGE}: {', '.join(copied)}, and the messages from {messages}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
