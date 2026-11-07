# VASDK_PYTHON_APP

A kitchen assistant in Python, on VASDK's Python binding: recipes step by step, timers that ring over anything, a shopping list, and cooking questions for an LLM with tools written in Python. Speech is recognized and spoken on this machine.

**The full guide: [docs/apps/python.md](../docs/apps/python.md).**

```bash
python VASDK_PYTHON_APP/app.py --text     # the agent alone, by text: no models, no LLM
python VASDK_PYTHON_APP/app.py --demo     # a scripted cook talks to it: the whole spoken loop, no microphone
python VASDK_PYTHON_APP/app.py            # talk to it (needs sounddevice)
ctest -R vasdk_python_kitchen             # its conversations and code
```

It needs VASDK built as a shared library (the default on Windows), which also builds `vasdk_providers`, the speech models CMake downloads, and its Python packages: `uv sync --project VASDK_PYTHON_APP --extra voice` ([`pyproject.toml`](pyproject.toml), [`uv.lock`](uv.lock)) or `pip install -r VASDK_PYTHON_APP/requirements.txt`. An LLM is optional: `LLM_ENDPOINT` and `LLM_MODEL` in `.env.local`.

| Path | |
| --- | --- |
| [`app.py`](app.py) | The app: text, typed, voice and demo modes |
| [`kitchen/`](kitchen/) | The kitchen in plain Python, and `Kitchen`, which wires it to VASDK |
| [`agent/`](agent/) | The conversation: a main file and its parts |
| [`config/`](config/), [`prompts/`](prompts/) | The engine config and the LLM's instructions |
| [`tests/`](tests/) | Conversations by text, and the Python tests |
| [`pyproject.toml`](pyproject.toml), [`uv.lock`](uv.lock), [`requirements.txt`](requirements.txt) | Its Python packages, for uv and for pip |
