"""The model store from Python, through the C API: a manifest of its own with file:// sources (so
it runs offline), pulled with progress, cancelled and resumed, verified, gated by its license, and
the models an engine config's providers load.

    VASDK_LIBRARY=build/cmake/c_library/libvasdk_c.so python3 bindings/python/tests/test_models.py
"""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vasdk  # noqa: E402

ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
MILLION_SHA256 = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"   # 1,000,000 x "a"


def main():
    assert vasdk.api_version() >= 1
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / "abc.txt").write_bytes(b"abc")
        (root / "million.bin").write_bytes(b"a" * 1000000)
        manifest = {"schema": 1, "models": [
            {"id": "small", "providers": ["vad/test"], "license": "MIT",
             "files": [{"path": "small/abc.txt", "url": (root / "abc.txt").as_uri(), "sha256": ABC_SHA256, "size": 3}]},
            {"id": "big", "providers": ["tts/test"], "license": "MIT",
             "files": [{"path": "big.bin", "url": (root / "million.bin").as_uri(), "sha256": MILLION_SHA256, "size": 1000000}]},
            {"id": "terms", "providers": [], "license": "CC-BY-NC-4.0", "requires_acceptance": True,
             "files": [{"path": "terms.txt", "url": (root / "abc.txt").as_uri(), "sha256": ABC_SHA256, "size": 3}]},
        ]}
        (root / "manifest.json").write_text(json.dumps(manifest))
        models = vasdk.Models(directory=root / "store", manifest_path=root / "manifest.json")
        assert Path(models.directory) == root / "store", models.directory

        listed = models.list()
        assert [model["id"] for model in listed] == ["small", "big", "terms"], listed
        assert listed[0]["status"] == "missing" and listed[1]["size"] == 1000000
        assert models.status("nothing") == "unknown"

        seen = []
        models.pull("small", progress=lambda model, file, done, total: seen.append((model, done, total)))
        assert models.status("small") == "present" and (root / "store" / "small" / "abc.txt").read_bytes() == b"abc"
        assert seen and seen[-1] == ("small", 3, 3), seen
        models.verify("small")

        # Stopped half way by the progress callback, then resumed
        try:
            models.pull("big", progress=lambda model, file, done, total: done < 500000)
            raise AssertionError("the pull wasn't stopped")
        except vasdk.VasdkError as error:
            assert "cancelled" in str(error), error
        assert models.status("big") == "partial"
        models.pull("big")
        models.verify("big")

        try:
            models.pull("terms")
            raise AssertionError("a license to accept was skipped")
        except vasdk.VasdkError as error:
            assert "CC-BY-NC-4.0" in str(error), error
        models.pull("terms", accept_license=True)

        (root / "engine.txtpb").write_text('providers { vad { id: "test" } tts { id: "test" } }\n')
        assert models.for_config(root / "engine.txtpb") == ["small", "big"]

        models.remove("small")
        assert models.status("small") == "missing"
        offline = vasdk.Models(directory=root / "store", manifest_path=root / "manifest.json", offline=True)
        try:
            offline.pull("small")
            raise AssertionError("an offline store downloaded")
        except vasdk.VasdkError as error:
            assert "offline" in str(error), error
        models.close()
        offline.close()

    # The built-in manifest
    builtin = vasdk.Models(directory=tempfile.gettempdir())
    ids = [model["id"] for model in builtin.list()]
    assert "silero_vad" in ids and "pocket_tts" in ids, ids
    assert isinstance(vasdk.Models.default_directory(), str)
    print("test_models: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
