# vasdk for Python

You can write and test VASDK agents from Python. The package wraps the C API (`VASDK/c/vasdk.h`) with ctypes, so there's nothing to compile. Python can supply:

- steps
- tools
- envelopes
- page types
- entity extractors

```python
import vasdk

registry = vasdk.Registry()

@registry.tool("save_booking")
def save_booking(args):
    return {"reference": "AG-1234", "status": "confirmed"}

@registry.entity_extractor("contacts")
def contacts(words, text):
    return [{"start": i, "value": "+15550100", "spoken": "Mom"} for i, word in enumerate(words) if word == "mom"]

agent = vasdk.Agent.load("agents/garage-menu/agent.txtpb", registry)
conversation = agent.conversation()
conversation.start()["text"]
conversation.say("book a tire repair")["page"]
vasdk.run_test_file(agent, "tests/book_service.txtpb")   # (True, "")
```

## A wheel

A wheel carries a shared build of VASDK for one platform. It works with any Python 3 there.

1. Build VASDK as a shared library. It is shared by default on Windows; elsewhere, configure with `-DBUILD_SHARED_LIBS=ON`.
2. Bundle the library and the libraries beside it into the package:

   ```sh
   python bindings/python/bundle.py build/VASDK/Release    # the folder with VASDK.dll or libVASDK.so
   ```

   On Windows this copies VASDK.dll and its dependencies: protobuf, abseil, curl, OpenSSL and zlib.

3. Build and install the wheel:

   ```sh
   pip wheel bindings/python -w dist                       # dist/vasdk-1.0.0-py3-none-win_amd64.whl
   pip install dist/vasdk-*.whl
   ```

To remove the copied libraries from the source tree, run `python bindings/python/bundle.py --clean`.

## Without a wheel

To use the package from the source tree instead, set `VASDK_LIBRARY` to the shared library. If it isn't set, the package looks in `./build/VASDK`.

```sh
VASDK_LIBRARY=build/VASDK/Release/VASDK.dll python bindings/python/tests/test_garage.py VASDK_APP/agents/garage-menu
```

The tests in `tests/` run in ctest as `vasdk_python_garage` and `vasdk_python_extensions`.
