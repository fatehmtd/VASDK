#pragma once

#include "vasdk_export.h"

#include <string>
#include <vector>
#include <cstdint>

#ifdef VASDK_HAS_JSON
#include <nlohmann/json.hpp>
#endif

// Forward-declare spdlog level so we avoid pulling the whole header in here.
namespace spdlog { namespace level { enum level_enum : int; } }

namespace va {
namespace metrics {

/**
 * Config for every SlidingWindowSampler instance.
 *
 * windowSize must be a power of 2 for the lock-free modulo to be cheap.
 * quantiles drives the number of P2Quantile estimators allocated per sampler.
 */
struct SamplerConfig {
    size_t              windowSize = 1024;               // ring buffer slot count
    std::vector<double> quantiles  = {0.5, 0.95, 0.99}; // P2Quantile instances per sampler
};

/**
 * Config for the AlertDispatcher background thread.
 *
 * queueCapacity is the bounded SPSC queue depth. When full, new alerts are
 * dropped silently - audio threads are never blocked.
 */
struct AlertDispatcherConfig {
    size_t queueCapacity = 64;
    bool   enabled       = true;
};

/**
 * Config for MetricsCollector registry.
 *
 * namePrefix is prepended to every metric name at registration time.
 * windowSize = 0 in a per-component config resolves to defaultSampler.windowSize.
 */
struct CollectorConfig {
    std::string          namePrefix       = "";
    size_t               registryCapacity = 256;  // reserved unordered_map buckets
    bool                 enabled          = true;
    SamplerConfig        defaultSampler;
    AlertDispatcherConfig alertDispatcher;
};

/**
 * Config for MetricsReporter periodic + per-utterance output.
 *
 * intervalSeconds = 0 disables the periodic report. Group flags control
 * which sections appear in each report.
 */
struct ReporterConfig {
    int  intervalSeconds        = 10;
    bool enablePeriodicReport   = true;
    bool enableUtteranceSummary = true;
    bool enableProcessorGroup   = true;  // per-processor latency / queue rows
    bool enableONNXGroup        = true;  // vad / turn / encoder / decoder rows
    bool enableQualityGroup     = true;  // confidence, word count, TTFT, TTFA rows
    bool enableTransportGroup   = true;  // transport counter rows
    bool enableAgentStateGroup  = true;  // state transition count rows
    int  logLevel               = 2;     // spdlog::level::info = 2
};

//  Per-component configs 

/**
 * Base metrics config for any FrameProcessor.
 *
 * processingWindow = 0 and queueWindow = 0 means "use CollectorConfig::defaultSampler.windowSize".
 * enabled = false means registration is skipped entirely in initialize() - zero overhead.
 */
struct ProcessorMetricsConfig {
    bool   enabled           = true;
    size_t processingWindow  = 0;  // 0 = collector default
    size_t queueWindow       = 0;
};

/** Extends processor config with VAD-specific sampler sizes and alert threshold. */
struct VADMetricsConfig : ProcessorMetricsConfig {
    size_t inferenceWindow  = 0;
    size_t speechDurWindow  = 0;
    double inferenceAlertMs = 0.0;  // 0 = no alert threshold
};

/** Turn detection processor metrics config. */
struct TurnDetectionMetricsConfig : ProcessorMetricsConfig {
    size_t inferenceWindow  = 0;
    double inferenceAlertMs = 0.0;
};

/** Transcription processor metrics config - one window per provider stage. */
struct TranscriptionMetricsConfig : ProcessorMetricsConfig {
    size_t encoderWindow    = 0;
    size_t decoderWindow    = 0;
    size_t confidenceWindow = 0;
    size_t wordCountWindow  = 0;
};

/** LLM processor metrics config. ttftAlertMs > 0 registers a Gauge threshold. */
struct LLMMetricsConfig : ProcessorMetricsConfig {
    size_t ttftWindow   = 0;
    double ttftAlertMs  = 0.0;
};

/** TTS processor metrics config. */
struct TTSMetricsConfig : ProcessorMetricsConfig {
    size_t ttfaWindow   = 0;
    size_t chunkWindow  = 0;
    double ttfaAlertMs  = 0.0;
};

/**
 * Transport metrics config.
 *
 * apiRoundtripWindow is used only by LLMTransport.
 * jitterWindow is used only by LocalTransport.
 */
struct TransportMetricsConfig {
    bool        enabled             = true;
    std::string namePrefix          = "";  // overrides getName()-based prefix when non-empty
    size_t      apiRoundtripWindow  = 0;
    size_t      jitterWindow        = 0;
};

//  Exporter configs 

/** PrometheusExporter HTTP server config. Compiled in only when VASDK_METRICS_PROMETHEUS=ON. */
struct PrometheusConfig {
    std::string bindAddress = "0.0.0.0";
    int         port        = 9099;
    bool        includeHelp = true;   // emit # HELP lines
    bool        includeType = true;   // emit # TYPE lines
};

/** StatsDMetricsTransport UDP config. */
struct StatsDConfig {
    std::string host            = "127.0.0.1";
    uint16_t    port            = 8125;
    std::string prefix          = "";    // prepended to all StatsD metric names
    int         flushIntervalMs = 1000;
};

/** InfluxMetricsTransport HTTP config. */
struct InfluxConfig {
    std::string url             = "http://localhost:8086";
    std::string org             = "";
    std::string bucket          = "vasdk";
    std::string token           = "";    // Authorization: Token <token>
    int         batchSize       = 500;
    int         flushIntervalMs = 5000;
};

/** WebSocketMetricsTransport config. pushIntervalMs = 0 = push only on utterance events. */
struct WebSocketMetricsConfig {
    std::string bindAddress    = "0.0.0.0";
    uint16_t    port           = 9100;
    size_t      maxConnections = 8;
    int         pushIntervalMs = 1000;
};

//  Top-level config 

/**
 * Top-level metrics configuration passed to MetricsCollector.
 *
 * Load from JSON first with fromJson(), then apply CLI overrides on individual
 * fields. toJson() logs the active config at startup.
 */
struct MetricsConfig {
    CollectorConfig            collector;
    ReporterConfig             reporter;
    // Per-component - each processor/transport calls setMetricsConfig() with its sub-config.
    ProcessorMetricsConfig     defaultProcessor;
    VADMetricsConfig           vad;
    TurnDetectionMetricsConfig turnDetection;
    TranscriptionMetricsConfig transcription;
    LLMMetricsConfig           llm;
    TTSMetricsConfig           tts;
    TransportMetricsConfig     transport;
    // Exporter configs - only used when the corresponding CMake flag is ON.
    PrometheusConfig           prometheus;
    StatsDConfig               statsd;
    InfluxConfig               influx;
    WebSocketMetricsConfig     websocket;

#ifdef VASDK_HAS_JSON
    static MetricsConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
#endif
};

#ifdef VASDK_HAS_JSON
//  JSON helpers 

inline MetricsConfig MetricsConfig::fromJson(const nlohmann::json& j)
{
    MetricsConfig cfg;
    auto get = [&](const char* key, auto& field) {
        if (j.contains(key) && !j[key].is_null()) field = j[key].get<std::remove_reference_t<decltype(field)>>();
    };

    if (j.contains("collector") && j["collector"].is_object()) {
        const auto& c = j["collector"];
        auto gc = [&](const char* k, auto& v) {
            if (c.contains(k) && !c[k].is_null()) v = c[k].get<std::remove_reference_t<decltype(v)>>();
        };
        gc("namePrefix",       cfg.collector.namePrefix);
        gc("registryCapacity", cfg.collector.registryCapacity);
        gc("enabled",          cfg.collector.enabled);
        if (c.contains("defaultSampler") && c["defaultSampler"].is_object()) {
            const auto& s = c["defaultSampler"];
            if (s.contains("windowSize")) cfg.collector.defaultSampler.windowSize = s["windowSize"];
            if (s.contains("quantiles"))  cfg.collector.defaultSampler.quantiles  = s["quantiles"].get<std::vector<double>>();
        }
        if (c.contains("alertDispatcher") && c["alertDispatcher"].is_object()) {
            const auto& a = c["alertDispatcher"];
            if (a.contains("queueCapacity")) cfg.collector.alertDispatcher.queueCapacity = a["queueCapacity"];
            if (a.contains("enabled"))       cfg.collector.alertDispatcher.enabled       = a["enabled"];
        }
    }

    auto loadReporter = [&]() {
        if (!j.contains("reporter") || !j["reporter"].is_object()) return;
        const auto& r = j["reporter"];
        auto gr = [&](const char* k, auto& v) {
            if (r.contains(k) && !r[k].is_null()) v = r[k].get<std::remove_reference_t<decltype(v)>>();
        };
        gr("intervalSeconds",        cfg.reporter.intervalSeconds);
        gr("enablePeriodicReport",   cfg.reporter.enablePeriodicReport);
        gr("enableUtteranceSummary", cfg.reporter.enableUtteranceSummary);
        gr("enableProcessorGroup",   cfg.reporter.enableProcessorGroup);
        gr("enableONNXGroup",        cfg.reporter.enableONNXGroup);
        gr("enableQualityGroup",     cfg.reporter.enableQualityGroup);
        gr("enableTransportGroup",   cfg.reporter.enableTransportGroup);
        gr("enableAgentStateGroup",  cfg.reporter.enableAgentStateGroup);
        gr("logLevel",               cfg.reporter.logLevel);
    };
    loadReporter();

    auto loadComponent = [&](const char* key, ProcessorMetricsConfig& p) {
        if (!j.contains(key) || !j[key].is_object()) return;
        const auto& c = j[key];
        if (c.contains("enabled"))           p.enabled          = c["enabled"];
        if (c.contains("processingWindow"))  p.processingWindow = c["processingWindow"];
        if (c.contains("queueWindow"))       p.queueWindow      = c["queueWindow"];
    };
    loadComponent("defaultProcessor", cfg.defaultProcessor);

    auto loadVAD = [&]() {
        loadComponent("vad", cfg.vad);
        if (!j.contains("vad") || !j["vad"].is_object()) return;
        const auto& c = j["vad"];
        if (c.contains("inferenceWindow"))  cfg.vad.inferenceWindow  = c["inferenceWindow"];
        if (c.contains("speechDurWindow"))  cfg.vad.speechDurWindow  = c["speechDurWindow"];
        if (c.contains("inferenceAlertMs")) cfg.vad.inferenceAlertMs = c["inferenceAlertMs"];
    };
    loadVAD();

    auto loadTurnDetection = [&]() {
        loadComponent("turnDetection", cfg.turnDetection);
        if (!j.contains("turnDetection") || !j["turnDetection"].is_object()) return;
        const auto& c = j["turnDetection"];
        if (c.contains("inferenceWindow"))  cfg.turnDetection.inferenceWindow  = c["inferenceWindow"];
        if (c.contains("inferenceAlertMs")) cfg.turnDetection.inferenceAlertMs = c["inferenceAlertMs"];
    };
    loadTurnDetection();

    auto loadTranscription = [&]() {
        loadComponent("transcription", cfg.transcription);
        if (!j.contains("transcription") || !j["transcription"].is_object()) return;
        const auto& c = j["transcription"];
        if (c.contains("encoderWindow"))    cfg.transcription.encoderWindow    = c["encoderWindow"];
        if (c.contains("decoderWindow"))    cfg.transcription.decoderWindow    = c["decoderWindow"];
        if (c.contains("confidenceWindow")) cfg.transcription.confidenceWindow = c["confidenceWindow"];
        if (c.contains("wordCountWindow"))  cfg.transcription.wordCountWindow  = c["wordCountWindow"];
    };
    loadTranscription();

    auto loadLLM = [&]() {
        loadComponent("llm", cfg.llm);
        if (!j.contains("llm") || !j["llm"].is_object()) return;
        const auto& c = j["llm"];
        if (c.contains("ttftWindow"))   cfg.llm.ttftWindow  = c["ttftWindow"];
        if (c.contains("ttftAlertMs"))  cfg.llm.ttftAlertMs = c["ttftAlertMs"];
    };
    loadLLM();

    auto loadTTS = [&]() {
        loadComponent("tts", cfg.tts);
        if (!j.contains("tts") || !j["tts"].is_object()) return;
        const auto& c = j["tts"];
        if (c.contains("ttfaWindow"))   cfg.tts.ttfaWindow  = c["ttfaWindow"];
        if (c.contains("chunkWindow"))  cfg.tts.chunkWindow = c["chunkWindow"];
        if (c.contains("ttfaAlertMs"))  cfg.tts.ttfaAlertMs = c["ttfaAlertMs"];
    };
    loadTTS();

    auto loadTransport = [&]() {
        if (!j.contains("transport") || !j["transport"].is_object()) return;
        const auto& c = j["transport"];
        if (c.contains("enabled"))            cfg.transport.enabled            = c["enabled"];
        if (c.contains("apiRoundtripWindow")) cfg.transport.apiRoundtripWindow = c["apiRoundtripWindow"];
        if (c.contains("jitterWindow"))       cfg.transport.jitterWindow       = c["jitterWindow"];
    };
    loadTransport();

    auto loadPrometheus = [&]() {
        if (!j.contains("prometheus") || !j["prometheus"].is_object()) return;
        const auto& c = j["prometheus"];
        if (c.contains("bindAddress")) cfg.prometheus.bindAddress = c["bindAddress"];
        if (c.contains("port"))        cfg.prometheus.port        = c["port"];
        if (c.contains("includeHelp")) cfg.prometheus.includeHelp = c["includeHelp"];
        if (c.contains("includeType")) cfg.prometheus.includeType = c["includeType"];
    };
    loadPrometheus();

    auto loadStatsD = [&]() {
        if (!j.contains("statsd") || !j["statsd"].is_object()) return;
        const auto& c = j["statsd"];
        if (c.contains("host"))            cfg.statsd.host            = c["host"];
        if (c.contains("port"))            cfg.statsd.port            = c["port"].get<uint16_t>();
        if (c.contains("prefix"))          cfg.statsd.prefix          = c["prefix"];
        if (c.contains("flushIntervalMs")) cfg.statsd.flushIntervalMs = c["flushIntervalMs"];
    };
    loadStatsD();

    auto loadInflux = [&]() {
        if (!j.contains("influx") || !j["influx"].is_object()) return;
        const auto& c = j["influx"];
        if (c.contains("url"))             cfg.influx.url             = c["url"];
        if (c.contains("org"))             cfg.influx.org             = c["org"];
        if (c.contains("bucket"))          cfg.influx.bucket          = c["bucket"];
        if (c.contains("token"))           cfg.influx.token           = c["token"];
        if (c.contains("batchSize"))       cfg.influx.batchSize       = c["batchSize"];
        if (c.contains("flushIntervalMs")) cfg.influx.flushIntervalMs = c["flushIntervalMs"];
    };
    loadInflux();

    auto loadWebSocket = [&]() {
        if (!j.contains("websocket") || !j["websocket"].is_object()) return;
        const auto& c = j["websocket"];
        if (c.contains("bindAddress"))    cfg.websocket.bindAddress    = c["bindAddress"];
        if (c.contains("port"))           cfg.websocket.port           = c["port"].get<uint16_t>();
        if (c.contains("maxConnections")) cfg.websocket.maxConnections = c["maxConnections"];
        if (c.contains("pushIntervalMs")) cfg.websocket.pushIntervalMs = c["pushIntervalMs"];
    };
    loadWebSocket();

    return cfg;
}

inline nlohmann::json MetricsConfig::toJson() const
{
    nlohmann::json j;

    j["collector"] = {
        {"namePrefix",        collector.namePrefix},
        {"registryCapacity",  collector.registryCapacity},
        {"enabled",           collector.enabled},
        {"defaultSampler",    {
            {"windowSize", collector.defaultSampler.windowSize},
            {"quantiles",  collector.defaultSampler.quantiles}
        }},
        {"alertDispatcher",   {
            {"queueCapacity", collector.alertDispatcher.queueCapacity},
            {"enabled",       collector.alertDispatcher.enabled}
        }}
    };

    j["reporter"] = {
        {"intervalSeconds",        reporter.intervalSeconds},
        {"enablePeriodicReport",   reporter.enablePeriodicReport},
        {"enableUtteranceSummary", reporter.enableUtteranceSummary},
        {"enableProcessorGroup",   reporter.enableProcessorGroup},
        {"enableONNXGroup",        reporter.enableONNXGroup},
        {"enableQualityGroup",     reporter.enableQualityGroup},
        {"enableTransportGroup",   reporter.enableTransportGroup},
        {"enableAgentStateGroup",  reporter.enableAgentStateGroup},
        {"logLevel",               reporter.logLevel}
    };

    auto procJson = [](const ProcessorMetricsConfig& p) -> nlohmann::json {
        return {{"enabled", p.enabled}, {"processingWindow", p.processingWindow}, {"queueWindow", p.queueWindow}};
    };
    j["defaultProcessor"] = procJson(defaultProcessor);
    j["vad"] = nlohmann::json::object();
    j["vad"].update(procJson(vad));
    j["vad"]["inferenceWindow"]  = vad.inferenceWindow;
    j["vad"]["speechDurWindow"]  = vad.speechDurWindow;
    j["vad"]["inferenceAlertMs"] = vad.inferenceAlertMs;
    j["turnDetection"] = nlohmann::json::object();
    j["turnDetection"].update(procJson(turnDetection));
    j["turnDetection"]["inferenceWindow"]  = turnDetection.inferenceWindow;
    j["turnDetection"]["inferenceAlertMs"] = turnDetection.inferenceAlertMs;
    j["transcription"] = nlohmann::json::object();
    j["transcription"].update(procJson(transcription));
    j["transcription"]["encoderWindow"]    = transcription.encoderWindow;
    j["transcription"]["decoderWindow"]    = transcription.decoderWindow;
    j["transcription"]["confidenceWindow"] = transcription.confidenceWindow;
    j["transcription"]["wordCountWindow"]  = transcription.wordCountWindow;
    j["llm"] = nlohmann::json::object();
    j["llm"].update(procJson(llm));
    j["llm"]["ttftWindow"]   = llm.ttftWindow;
    j["llm"]["ttftAlertMs"]  = llm.ttftAlertMs;
    j["tts"] = nlohmann::json::object();
    j["tts"].update(procJson(tts));
    j["tts"]["ttfaWindow"]   = tts.ttfaWindow;
    j["tts"]["chunkWindow"]  = tts.chunkWindow;
    j["tts"]["ttfaAlertMs"]  = tts.ttfaAlertMs;
    j["transport"] = {
        {"enabled",            transport.enabled},
        {"apiRoundtripWindow", transport.apiRoundtripWindow},
        {"jitterWindow",       transport.jitterWindow}
    };
    j["prometheus"] = {
        {"bindAddress", prometheus.bindAddress},
        {"port",        prometheus.port},
        {"includeHelp", prometheus.includeHelp},
        {"includeType", prometheus.includeType}
    };
    j["statsd"] = {
        {"host",            statsd.host},
        {"port",            statsd.port},
        {"prefix",          statsd.prefix},
        {"flushIntervalMs", statsd.flushIntervalMs}
    };
    j["influx"] = {
        {"url",             influx.url},
        {"org",             influx.org},
        {"bucket",          influx.bucket},
        {"token",           influx.token},
        {"batchSize",       influx.batchSize},
        {"flushIntervalMs", influx.flushIntervalMs}
    };
    j["websocket"] = {
        {"bindAddress",    websocket.bindAddress},
        {"port",           websocket.port},
        {"maxConnections", websocket.maxConnections},
        {"pushIntervalMs", websocket.pushIntervalMs}
    };
    return j;
}
#endif // VASDK_HAS_JSON

} // namespace metrics
} // namespace va
