# Context providers and metrics

*Reference for `va::context` and `va::metrics`. Headers in [`context/`](../../VASDK/include/VASDK/context/) and [`metrics/`](../../VASDK/include/VASDK/metrics/) are the source of truth if this page and the code ever disagree.*

## Context - what the assistant knows about its surroundings

An LLM answering "how's my battery?" or "where's the nearest charger?" needs facts about the device and situation. The `context/` namespace is a small, uniform way to collect those facts and inject them into prompts.

**`ContextProvider`** ([`ContextProvider.h`](../../VASDK/include/VASDK/context/ContextProvider.h)) is the base: a thread-safe key/value store backed by a JSON object - `setValue(key, json)`, `getValue(key)`, `removeValue(key)`, `containsKey(key)`, and `getContext()` for the whole object. It's deliberately dumb: subclasses are just namespaced conventions over which keys they maintain.

| Subclass | Maintains |
| --- | --- |
| `AppContextProvider` | app id, name, version, build number |
| `SystemContextProvider` | device, battery, memory, CPU |
| `NetworkContextProvider` | Wi-Fi, cellular, connection quality |
| `LocationContextProvider` | location context |
| `ToolsContextProvider` | available tools and their enabled state |

**`ContextAggregator`** ([`ContextAggregator.h`](../../VASDK/include/VASDK/context/ContextAggregator.h)) is itself a `ContextProvider` that merges multiple named providers: `setContextFromProvider("system", systemProvider)` registers each source, and its `getContext()` returns the combined JSON document. That makes it the natural payload for an [`LLMTransport` template variable](llm.md#llmtransport) resolver, so the prompt always carries fresh context without any per-turn plumbing:

```cpp
transport->registerTemplateVar("device_context", [agg](va::llm::ChatFormat) {
    return agg->getContext().dump();
});
```

The population side is platform code (the Android app feeds these from Android APIs; a vehicle integration would feed them from the CAN bus / head unit). A custom context source is just another `ContextProvider` subclass that writes its keys on whatever schedule makes sense.

## Metrics - what the pipeline can tell you about itself

> Status honesty (same as the [root README](../../README.md#metrics)): the metrics layer is a solid base for basic visibility, but not yet thoroughly tested as a production observability path. Validate numbers you intend to alert on.

The design goal is **zero-cost-when-off, cheap-when-on** instrumentation of a real-time audio path:

- **`MetricsCollector`** - the registry. Processors and transports register named instruments at `initialize()` time and get back lightweight **handles** (`CounterHandle`, `GaugeHandle`, `SamplerHandle` - see `MetricHandles.h`); the hot path just writes through a handle, no lookup, no lock contention on the audio thread.
- **`MetricsConfig`** - per-stage config structs (`ProcessorMetricsConfig`, `VADMetricsConfig`, `LLMMetricsConfig`, `TTSMetricsConfig`, `TranscriptionMetricsConfig`, `TransportMetricsConfig`, …) passed via each processor's `setMetricsConfig()`. This is also where alert thresholds live (e.g. LLM time-to-first-token).
- **`MetricsReporter`** - periodic console reporting (the default you see running `VASDK_Pipeline`).
- **Exporters** (`metrics/exporters/`, each behind a CMake option): Prometheus HTTP (`VASDK_METRICS_PROMETHEUS`), StatsD UDP (`VASDK_METRICS_STATSD`), InfluxDB line protocol (`VASDK_METRICS_INFLUX`), WebSocket push (`VASDK_METRICS_WEBSOCKET`).

What gets measured out of the box: every processor's processing latency, queue wait, and frame count (from `FrameProcessor` base); stage-specific samplers like VAD inference latency and speech duration, STT encoder/decoder latency (via `TimingObserverInterface`), LLM time-to-first-token and token counts, TTS time-to-first-audio; transport frame/byte/error/dropped counters; capture callback jitter and underruns.

Two higher-level observers matter for the call center use case:

- **`CallMetrics`** (`CallMetrics.h`) - per-call KPIs, including per-slot collection tracking when attached to a [`GoalManager`](goal.md#goalmanager--the-wiring-hub) via `setMetrics()`.
- **`UtteranceObserverInterface`** (`UtteranceObserver.h`) - end-to-end per-utterance timing, attached to `AgentStateProcessor::setUtteranceObserver()`.

Runtime flags for all of this (`--metrics`, `--metrics-interval`, `--prometheus`, `--statsd-host`, alert thresholds, or a full JSON config via `--metrics-config`): see the [VASDK_Pipeline flags](../../VASDK_APP/README.md#cli-flags). A ready-made Prometheus + Grafana stack ships in the repo's `docker-compose` setup ([root README, Metrics](../../README.md#metrics)).
