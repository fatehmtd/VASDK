# VASDK_Car

An in-car voice assistant and its screen, built from ten self-contained verticals: drive, climate, media, phone, messages, navigation, driver profiles, the owner's manual, a companion for stories and games, and general knowledge. Commands are rules that run on the device with no LLM; an LLM, when there is one, does the rest with the car's tools.

**The full guide: [docs/apps/car.md](../docs/apps/car.md).**

```bash
cmake --build build --config Release --target VASDK_Car
./build/VASDK_CAR_APP/Release/VASDK_Car --webrtc --env-file .env.local   # then open http://127.0.0.1:8090
ctest -R vasdk_car                                                       # every vertical's conversations and code
```

| Folder | |
| --- | --- |
| [`agents/car/`](agents/car/) | The start page and the list of parts (`include_files`), shared commands, general tests |
| [`verticals/<name>/`](verticals/) | One vertical: its code, its part of the agent, its tests, its panel, its part of the LLM's instructions |
| [`core/`](core/) | `Vertical`, `Car` and the list of verticals; `CarTest.h` for code tests |
| [`dashboard/`](dashboard/) | The page's shell and its server |
| [`config/`](config/), [`prompts/`](prompts/) | The engine config (providers, LLM profiles) and the LLM's instructions |
