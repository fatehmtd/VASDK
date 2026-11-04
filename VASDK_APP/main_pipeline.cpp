#include <VASDK/profiling/CTrack.h>
#include <chrono>
#include <ctime>
#include <VASDK/Pipeline.h>
#include <VASDK/VAEngine.h>
#include <VASDK/flow/CompiledAgent.h>
#include "GarageExtensions.h"
#include "vasdk/engine/events.pb.h"
#include <VASDK/llm/Tool.h>
#include <VASDK/FrameProcessor.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/WebRtcTransport.h>
#include <VASDK/AgentStateProcessor.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <VASDK/metrics/MetricsConfig.h>
#include <VASDK/metrics/MetricsReporter.h>
#ifdef VASDK_HAS_JSON
#include <VASDK/SessionWriter.h>
#include <VASDK/SessionRecorder.h>
#endif
#ifdef VASDK_METRICS_STATSD
#include <VASDK/metrics/exporters/StatsDMetricsTransport.h>
#endif
#ifdef VASDK_METRICS_INFLUX
#include <VASDK/metrics/exporters/InfluxMetricsTransport.h>
#endif
#ifdef VASDK_METRICS_WEBSOCKET
#include <VASDK/metrics/exporters/WebSocketMetricsTransport.h>
#endif
#ifdef VASDK_METRICS_PROMETHEUS
#include <VASDK/metrics/exporters/PrometheusExporter.h>
#endif

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <dotenv.h>
#include <spdlog/sinks/callback_sink.h>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Audio I/O
#ifdef VASDK_HAS_MINIAUDIO
#include "MiniAudio/MiniAudioSource.h"
#include "MiniAudio/MiniAudioSink.h"
#elif defined(VASDK_HAS_SDL)
#include "SDL/SDLAudioSource.h"
#include "SDL/SDLAudioSink.h"
#endif

// Transports
#ifdef VASDK_HAS_FFMPEG_AUDIO
#include "FFmpegAudio/FileTransport.h"
#endif

// Engine config + provider catalog (VAD, STT, TTS and turn detection are picked by id from the config)
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <VASDK/engine/Runtime.h>
#include <vasdk/engine/engine_config.pb.h>
#include "ProviderCatalog/ProviderCatalog.h"

// Speaker identification
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
#include <VASDK/SpeakerIdentificationProcessor.h>
#include "PyannoteSpeaker/PyannoteEmbeddingProvider.h"
#endif

#ifdef VASDK_HAS_LLAMACPP
#include <VASDK/llm/ToolDispatchProcessor.h>
#include "LlamaCpp/LlamaCppTransport.h"
#endif

#ifdef VASDK_HAS_VIZANIM
#include "VizAnim/VizAnimProvider.h"
#include "VizAnim/VizAnimFrameProcessor.h"
#include "VizAnim/AudioFeaturesEvent.h"
#endif

#ifdef VASDK_HAS_FTXUI
#include "TUI/PipelineTUI.h"
#endif

std::atomic<bool> running{ true };

static std::string loadTextFile(const std::string& path) {
    if (path.empty()) return "";
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("[Pipeline] Could not open prompt file: {}", path);
        return "";
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::string loadOrFallback(const std::string& path, const char* fallback) {
    if (path.empty()) return fallback;
    auto s = loadTextFile(path);
    return s.empty() ? fallback : s;
}

static constexpr const char* kResponseFormatInstruction =
    "RESPONSE FORMAT - always reply with valid JSON, nothing else:\n"
    "{\"text\": \"<spoken response>\", \"isFinal\": true}\n\n"
    "Add \"isExpecting\": true when you are asking the user a question:\n"
    "{\"text\": \"<question>\", \"isFinal\": true, \"isExpecting\": true}\n\n"
    "Add a \"choices\" array when the user must pick from a fixed set of options:\n"
    "{\"text\": \"<question>\", \"isFinal\": true, \"isExpecting\": true, "
    "\"choices\": [{\"text\": \"Option A\"}, {\"text\": \"Option B\"}]}\n\n"
    "The \"text\" value is spoken aloud - keep it natural. "
    "Always put \"text\" first, and open it with a short sentence: it is spoken while the rest is still being written. "
    "Never add keys outside this schema or wrap the JSON in markdown.";

class WeatherTool : public va::llm::Tool {
    public:
    WeatherTool() : Tool("get_weather", "Get current weather for a specified location") {
        addParameter({"location", "Location to get weather for", "string", true});
        onCall([this](const nlohmann::json& args) -> nlohmann::json {
            std::string location = args.value("location", "unknown");
            spdlog::info("[WeatherTool] Called with location: {}", location);
            nlohmann::json result;
            result["location"] = location;
            result["temperature"] = "20°C";
            result["condition"] = "Sunny";
            return result;
        });
    }
};

class TimeTool : public va::llm::Tool {
    public:
    TimeTool() : Tool("get_time", "Get current time for a specified location") {
        addParameter({"location", "Location to get time for", "string", true});
        onCall([this](const nlohmann::json& args) -> nlohmann::json {
            std::string location = args.value("location", "unknown");
            spdlog::info("[TimeTool] Called with location: {}", location);
            nlohmann::json result;
            result["location"] = location;
            result["current_time"] = "2024-01-01T12:00:00Z";
            return result;
        });
    }
};


// A live console of the conversation, from the engine's events.
void addConsoleListener(va::VAEngine& engine) {
    engine.addListener([](const vasdk::engine::Event& event) {
        switch (event.kind_case()) {
        case vasdk::engine::Event::kTranscript:
            if (event.transcript().is_final())
                spdlog::info("you: {}{}", event.transcript().text(), event.transcript().is_backchannel() ? " (back-channel)" : "");
            break;
        case vasdk::engine::Event::kAgentReply:
            if (event.agent_reply().is_final()) spdlog::info("assistant: {}", event.agent_reply().text());
            break;
        case vasdk::engine::Event::kAgentState:
            spdlog::info("[{} → {}] {}", event.agent_state().previous_state(), event.agent_state().state(),
                         event.agent_state().transition());
            break;
        case vasdk::engine::Event::kToolCall:
            spdlog::info("[tool] {}({}) → {}", event.tool_call().name(), event.tool_call().arguments_json(),
                         event.tool_call().result_json());
            break;
        case vasdk::engine::Event::kError:
            spdlog::error("[{}] {}", event.error().source(), event.error().message());
            break;
        case vasdk::engine::Event::kPageChanged:
            spdlog::info("[page {}/{} → {}/{}] {}", event.page_changed().from_flow(), event.page_changed().from_page(),
                         event.page_changed().to_flow(), event.page_changed().to_page(), event.page_changed().reason());
            break;
        case vasdk::engine::Event::kSessionEnded:
            spdlog::info("[conversation ended on {}/{}]", event.session_ended().flow(), event.session_ended().page());
            break;
        default:
            break;
        }
    });
}

void signalHandler(int signal) {
    spdlog::info("Received signal {}, shutting down...", signal);
    running = false;
}

/**
 * @brief Search for the models directory by walking up from the executable path
 * @param argv Command line arguments (used to get executable path)
 * @param expectedSubdir The expected relative path to the models directory from the executable
 * @param maxLevels Maximum number of parent directories to search
 */
std::string findModelsDirectory(char* argv[],
    const char* expectedSubdir = "VASDK_3P_IMPL/models",
    int maxLevels = 3) {
    std::filesystem::path searchDir = std::filesystem::canonical(std::filesystem::path(argv[0])).parent_path();
    for (int i = 0; i <= maxLevels; ++i) {
        auto candidate = searchDir / expectedSubdir;
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
        if (!searchDir.has_parent_path() || searchDir == searchDir.parent_path()) break;
        searchDir = searchDir.parent_path();
    }
    return "";
}

#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
#include "LibDataChannelWebRtc/LibDataChannelWebRtcProvider.h"
#endif

//  Data structs 

struct CliArgs {
    std::string engineConfigPath;
    std::string agentPath;
    std::string llmModelPath;
    std::string llmEndpoint;
    std::string llmApiKey;
    std::string llmModel;
    std::string systemPromptFile;
    std::string formatInstructionFile;
    std::string envFile           = ".env";
    bool        metricsEnabled     = true;
    int         metricsIntervalSec = 10;
    std::string metricsConfigFile;
    double      vadAlertMs     = 0.0;
    double      llmTtftAlertMs = 0.0;
    double      ttsAlertMs     = 0.0;
    std::string sessionOutputDir;
    std::string statsdHost;
    uint16_t    statsdPort        = 8125;
    bool        prometheusEnabled = false;
    int         prometheusPort    = 9099;
    uint16_t    webrtcPort        = 0;     // 0 = local mic/speaker
    va::pipeline::BackchannelConfig backchannelConfig;  // speech over the agent: back-channel vs barge-in
    std::string webrtcPage;
};

struct MetricsBundle {
    va::metrics::MetricsConfig                           config;
    std::shared_ptr<va::metrics::MetricsReporter>        reporter;
#ifdef VASDK_METRICS_STATSD
    std::shared_ptr<va::metrics::StatsDMetricsTransport> statsdExporter;
#endif
#ifdef VASDK_METRICS_PROMETHEUS
    std::shared_ptr<va::metrics::PrometheusExporter>     prometheusExporter;
#endif
};

//  Helper functions 

CliArgs parseCliArgs(int argc, char* argv[]) {
    CliArgs args;
    CLI::App app("VASDK Voice Assistant Pipeline");
    app.allow_extras();
#ifdef VASDK_HAS_LLAMACPP
    app.add_option("--llm-model-path", args.llmModelPath,
        "Path to GGUF model file for llama.cpp local LLM inference. "
        "When provided, overrides the default OpenAI-compatible transport.");
#endif
    app.add_option("--config", args.engineConfigPath,
        "Engine config file (.txtpb, .json or .pb): providers, models dir and LLM settings. "
        "Command-line LLM options override it.");
    app.add_option("--agent", args.agentPath,
        "Agent file (.txtpb, .json or .pb): its pages answer each turn, before any LLM, "
        "e.g. agents/garage-menu/agent.txtpb");
    app.add_option("--llm-endpoint", args.llmEndpoint,
        "OpenAI-compatible LLM endpoint URL (falls back to LLM_ENDPOINT / OPENAI_BASE_URL env vars)");
    app.add_option("--llm-api-key", args.llmApiKey,
        "LLM API key (falls back to LLM_API_KEY / OPENROUTER_API_KEY / OPENAI_API_KEY env vars)");
    app.add_option("--llm-model",   args.llmModel,
        "LLM model name (falls back to LLM_MODEL env var)");
    app.add_flag("--metrics,!--no-metrics", args.metricsEnabled,
        "Enable or disable all metrics collection (default: on)");
    app.add_option("--metrics-interval", args.metricsIntervalSec,
        "Periodic metrics report interval in seconds (default: 10)");
    app.add_option("--metrics-config", args.metricsConfigFile,
        "Path to a JSON file with full metrics configuration (overrides CLI flags)");
    app.add_option("--vad-alert-ms", args.vadAlertMs,
        "VAD inference latency alert threshold in ms (0 = disabled)");
    app.add_option("--llm-ttft-alert-ms", args.llmTtftAlertMs,
        "LLM time-to-first-token alert threshold in ms (0 = disabled)");
    app.add_option("--tts-alert-ms", args.ttsAlertMs,
        "TTS time-to-first-audio alert threshold in ms (0 = disabled)");
    app.add_option("--session-dir", args.sessionOutputDir,
        "Directory for session recording output (omit to disable recording)");
    app.add_option("--system-prompt", args.systemPromptFile,
        "Path to a text file containing the LLM system prompt (overrides built-in default)");
    app.add_option("--format-instruction", args.formatInstructionFile,
        "Path to a text file containing the response format instruction (overrides built-in default)");
    app.add_flag("--backchannel,!--no-backchannel", args.backchannelConfig.enabled,
        "Let back-channels (\"mm-hmm\", \"yeah\") pass without interrupting the agent (default: on)");
    app.add_option("--backchannel-playback", args.backchannelConfig.pendingPlaybackMode,
        "Agent playback while speech over it is classified: duck, pause or continue (default: duck)")
        ->transform(CLI::CheckedTransformer(std::map<std::string, va::transport::PendingPlaybackMode>{
            {"duck", va::transport::PendingPlaybackMode::Duck},
            {"pause", va::transport::PendingPlaybackMode::Pause},
            {"continue", va::transport::PendingPlaybackMode::Continue}}, CLI::ignore_case));
    app.add_option("--duck-gain-db", args.backchannelConfig.duckGainDecibels,
        "Assistant level while ducked for speech over it, in dB (default: -12)")
        ->check(CLI::Range(-60.0f, 0.0f));
    app.add_option("--duck-duration-ms", args.backchannelConfig.duckDurationMs,
        "Minimum time the assistant stays ducked once speech over it starts, in ms (default: 2500)");
    app.add_option("--backchannel-history", args.backchannelConfig.historyMode,
        "Dismissed back-channels: drop, or append them to the user's next message (default: drop)")
        ->transform(CLI::CheckedTransformer(std::map<std::string, va::pipeline::BackchannelHistoryMode>{
            {"drop", va::pipeline::BackchannelHistoryMode::Drop},
            {"append", va::pipeline::BackchannelHistoryMode::AppendToHistory}}, CLI::ignore_case));
    app.add_option("--tail-overlap", args.backchannelConfig.tailOverlapMode,
        "Speech over the end of a reply: turn (answer it) or classify (default: turn)")
        ->transform(CLI::CheckedTransformer(std::map<std::string, va::pipeline::TailOverlapMode>{
            {"turn", va::pipeline::TailOverlapMode::TreatAsTurn},
            {"classify", va::pipeline::TailOverlapMode::Classify}}, CLI::ignore_case));
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    app.add_option("--webrtc-port", args.webrtcPort,
        "Talk over WebRTC instead of the local mic/speaker: serve signaling + demo page on "
        "http://127.0.0.1:<port>");
    app.add_option("--webrtc-page", args.webrtcPage, "Demo page served at / in WebRTC mode")
        ->default_val(VASDK_WEBRTC_DEMO_PAGE);
#endif
    app.add_option("--env-file", args.envFile,
        "Path to .env file with LLM_API_KEY / LLM_ENDPOINT / LLM_MODEL (default: .env)");
#ifdef VASDK_METRICS_STATSD
    app.add_option("--statsd-host", args.statsdHost, "StatsD UDP host for metrics export");
    app.add_option("--statsd-port", args.statsdPort, "StatsD UDP port (default: 8125)");
#endif
#ifdef VASDK_METRICS_PROMETHEUS
    app.add_flag("--prometheus,!--no-prometheus", args.prometheusEnabled,
        "Enable Prometheus HTTP metrics endpoint (default: off)");
    app.add_option("--prometheus-port", args.prometheusPort,
        "Prometheus HTTP scrape port (default: 9099)");
#endif
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // --help, --version, or bad arguments: print and exit like CLI11_PARSE would in main()
        std::exit(app.exit(e));
    }
    return args;
}

MetricsBundle setupMetrics(const CliArgs& args) {
    MetricsBundle m;
    m.config.collector.enabled               = args.metricsEnabled;
    m.config.reporter.intervalSeconds        = args.metricsIntervalSec;
    m.config.reporter.enablePeriodicReport   = args.metricsEnabled;
    m.config.reporter.enableUtteranceSummary = args.metricsEnabled;
    m.config.vad.enabled                     = args.metricsEnabled;
    m.config.vad.inferenceAlertMs            = args.vadAlertMs;
    m.config.turnDetection.enabled           = args.metricsEnabled;
    m.config.transcription.enabled           = args.metricsEnabled;
    m.config.llm.enabled                     = args.metricsEnabled;
    m.config.llm.ttftAlertMs                 = args.llmTtftAlertMs;
    m.config.tts.enabled                     = args.metricsEnabled;
    m.config.tts.ttfaAlertMs                 = args.ttsAlertMs;
    m.config.transport.enabled               = args.metricsEnabled;
    m.config.defaultProcessor.enabled        = args.metricsEnabled;

#ifdef VASDK_HAS_JSON
    if (!args.metricsConfigFile.empty()) {
        std::ifstream f(args.metricsConfigFile);
        if (f.is_open()) {
            m.config = va::metrics::MetricsConfig::fromJson(nlohmann::json::parse(f));
            spdlog::info("Loaded metrics config from {}", args.metricsConfigFile);
        }
        else {
            spdlog::warn("Could not open metrics config file: {}", args.metricsConfigFile);
        }
    }
#endif

    // Apply collector config to the singleton before any processor is constructed,
    // so enabled/namePrefix/registryCapacity settings are honoured.
    va::metrics::MetricsCollector::setInstanceConfig(m.config.collector);
    auto& globalCollector = va::metrics::MetricsCollector::instance();
    m.reporter = std::make_shared<va::metrics::MetricsReporter>(globalCollector, m.config.reporter);

#ifdef VASDK_METRICS_STATSD
    if (!args.statsdHost.empty()) {
        va::metrics::StatsDConfig sdCfg = m.config.statsd;
        sdCfg.host = args.statsdHost;
        sdCfg.port = args.statsdPort;
        m.statsdExporter = std::make_shared<va::metrics::StatsDMetricsTransport>(globalCollector, sdCfg);
        m.reporter->addExporter(m.statsdExporter);
        if (args.metricsEnabled) m.statsdExporter->start();
        spdlog::info("StatsD metrics export enabled → {}:{}", args.statsdHost, args.statsdPort);
    }
#endif
#ifdef VASDK_METRICS_PROMETHEUS
    if (args.prometheusEnabled) {
        va::metrics::PrometheusConfig promCfg = m.config.prometheus;
        promCfg.port = args.prometheusPort;
        m.prometheusExporter = std::make_shared<va::metrics::PrometheusExporter>(globalCollector, promCfg);
        m.reporter->addExporter(m.prometheusExporter);
        if (args.metricsEnabled) {
            m.prometheusExporter->start();
            spdlog::info("Prometheus metrics endpoint enabled on http://{}:{}/metrics",
                promCfg.bindAddress, promCfg.port);
        }
        else {
            spdlog::warn("Prometheus endpoint requested but metrics are disabled (--no-metrics)");
        }
    }
#endif
    return m;
}

static bool loadDotenvFile(const std::string& path) {
    std::filesystem::path envPath = path;
    bool exists = std::filesystem::exists(envPath);
    std::string absPath = std::filesystem::weakly_canonical(std::filesystem::absolute(envPath)).string();
    if (exists) {
        dotenv::init(path.c_str());
        spdlog::info("  Env file:    {} (loaded)", absPath);
        return true;
    }
    spdlog::warn("  Env file:    {} (not found - skipped)", absPath);
    return false;
}

// CLI arg → env var fallback helpers (env vars are populated from .env by dotenv::init())

static std::string resolveLlmApiKey(const std::string& fromCli = "") {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_API_KEY"))        return v;
    if (const char* v = std::getenv("OPENROUTER_API_KEY")) return v;
    if (const char* v = std::getenv("OPENAI_API_KEY"))     return v;
    return "";
}

static std::string resolveLlmEndpoint(const std::string& fromCli = "") {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_ENDPOINT"))    return v;
    if (const char* v = std::getenv("OPENAI_BASE_URL")) return v;
    return "https://openrouter.ai/api/v1/chat/completions";
}

static std::string resolveLlmModel(const std::string& fromCli = "") {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_MODEL")) return v;
    return "";
}


#ifdef VASDK_HAS_LLAMACPP
// A GGUF model run in-process by llama.cpp, instead of the OpenAI-compatible endpoint.
std::shared_ptr<va::llm::LLMTransport> createLocalLlm(const std::string& modelPath, const std::string& systemPrompt) {
    va::llm::LLMConfig config;
    config.model             = modelPath;
    config.temperature       = 0.1f;
    config.maxTokens         = 8000;
    config.nCtx              = 16000;
    config.topK              = 50;
    config.repetitionPenalty = 1.05f;
    // LlamaCppTransport reads its history directly, so the tool descriptions must be in the
    // system prompt: the PromptTemplate resolver is never called.
    va::llm::ToolDispatchProcessor tools;
    tools.registerTool(WeatherTool{});
    tools.registerTool(TimeTool{});
    config.systemPrompt = systemPrompt + "\n\n" + tools.renderToolDescriptions(va::llm::ChatFormat::LFM2);
    auto transport = std::make_shared<va::transport::LlamaCppTransport>(config);
    transport->setChunker(std::make_shared<va::transport::NoChunkingChunker>());
    spdlog::info("LLM: llama.cpp local model {}", modelPath);
    return transport;
}
#endif

// Mic and speaker, or a browser over WebRTC (--webrtc-port).
struct AudioIO {
    std::shared_ptr<va::transport::AudioTransport> transport;
    // Local speaker only: called with each block of audio as it is played.
    std::function<void(std::function<void(const char*, size_t)>)> setPlaybackCallback;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    std::shared_ptr<va::webrtc::LibDataChannelWebRtcProvider> webrtcPeer;
#endif
};

AudioIO createAudioIO([[maybe_unused]] const CliArgs& args, const va::audio::AudioConfig& inputConfig) {
    AudioIO io;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (args.webrtcPort != 0) {
        io.webrtcPeer = std::make_shared<va::webrtc::LibDataChannelWebRtcProvider>();
        auto webrtc = std::make_shared<va::transport::WebRtcTransport>(io.webrtcPeer);
#ifdef VASDK_HAS_VIZANIM
        // The VizAnim processors analyse mic and TTS audio at the input rate.
        webrtc->setEventEncoder(va::transport::FrameType::AUDIO_FEATURES,
                                va::thirdparty::audioFeaturesEventEncoder(inputConfig.sampleRate));
#endif
        io.transport = webrtc;
        return io;
    }
#endif
#ifdef VASDK_HAS_MINIAUDIO
    auto sink   = std::make_shared<va::miniaudio::MiniAudioSink>();
    auto source = std::make_shared<va::miniaudio::MiniAudioSource>(
        inputConfig.sampleRate * inputConfig.channels * inputConfig.getSampleSizeInBytes() * 30);
#elif defined(VASDK_HAS_SDL)
    auto sink   = std::make_shared<SDLAudioSink>();
    auto source = std::make_shared<SDLAudioSource>();
#else
#error "No audio I/O backend available. Enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL."
#endif
    io.setPlaybackCallback = [sink](auto callback) { sink->setPlaybackCallback(std::move(callback)); };
    io.transport = std::make_shared<va::transport::LocalTransport>(source, sink);
    return io;
}

// What the app adds around the engine's pipeline: logging, recording, visualisers, the TUI.
struct AppProcessors {
#ifdef VASDK_HAS_JSON
    std::shared_ptr<va::pipeline::SessionWriter> sessionWriter;
#endif
#ifdef VASDK_HAS_FTXUI
    std::shared_ptr<vasdk_app::AudioFeaturesObserver> tuiObserver;
#endif
};

AppProcessors addAppProcessors(va::VAEngine& engine, const CliArgs& args, const AudioIO& io,
                               [[maybe_unused]] const std::string& modelsPath,
                               const va::audio::AudioConfig& inputConfig,
                               [[maybe_unused]] const va::audio::AudioConfig& outputConfig) {
    using Slot = va::VAEngine::Slot;
    AppProcessors app;
    const auto micAudio = engine.inputRingBuffer();

#ifdef VASDK_HAS_JSON
    if (!args.sessionOutputDir.empty()) {
        app.sessionWriter = std::make_shared<va::pipeline::SessionWriter>(args.sessionOutputDir);
        auto audioRecorder = std::make_shared<va::pipeline::SessionRecorder>(app.sessionWriter, "AudioRecorder");
        audioRecorder->setRingBuffer(micAudio);
        engine.addProcessor(Slot::AfterInput, audioRecorder);
        engine.addProcessor(Slot::BeforeSynthesis,
                            std::make_shared<va::pipeline::SessionRecorder>(app.sessionWriter, "SemanticRecorder"));
        spdlog::info("Session recording enabled → {}", app.sessionWriter->getSessionDir());
    }
#endif
    engine.addProcessor(Slot::AfterInput, std::make_shared<va::thirdparty::AudioInputVizAnimFrameProcessor>(
        micAudio, inputConfig, inputConfig.sampleRate * 10));

#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // After STT: embedding the whole utterance takes ~70 ms per second of speech.
    auto speakerProvider = std::make_shared<va::thirdparty::PyannoteEmbeddingProvider>(
        modelsPath + "/pyannote_embedding.onnx");
    if (speakerProvider->initialize(inputConfig)) {
        auto speakerId = std::make_shared<va::pipeline::SpeakerIdentificationProcessor>(speakerProvider);
        speakerId->setRingBuffer(micAudio);
        engine.addProcessor(Slot::AfterTranscription, speakerId);
    } else {
        spdlog::error("Failed to initialize PyannoteEmbeddingProvider - speaker tracking disabled");
    }
#endif

    engine.addProcessor(Slot::BeforeOutput, std::make_shared<va::thirdparty::OutputAudioVizAnimFrameProcessor>(
        inputConfig, inputConfig.sampleRate * 10));
#ifdef VASDK_HAS_FTXUI
    app.tuiObserver = std::make_shared<vasdk_app::AudioFeaturesObserver>();
    app.tuiObserver->setSynthAudioConfig(outputConfig);
    if (io.setPlaybackCallback) {
        io.setPlaybackCallback([observer = std::weak_ptr<vasdk_app::AudioFeaturesObserver>(app.tuiObserver)](
                                   const char* data, size_t bytes) {
            if (auto tuiObserver = observer.lock()) tuiObserver->onSynthPlayback(data, bytes);
        });
    }
    engine.addProcessor(Slot::BeforeOutput, app.tuiObserver);
#else
    (void)io;
#endif
    return app;
}

void runMainLoop(const MetricsBundle& metrics, const va::VAEngine& engine, const std::string& llmModel,
                 [[maybe_unused]] const AppProcessors& app) {
    // Start reporter after pipeline is running so all metrics are registered
    // and the first periodic tick has real data (default interval: 10s).
    if (metrics.config.collector.enabled) {
        metrics.reporter->start();
        spdlog::info("Metrics active - reporting every {}s", metrics.config.reporter.intervalSeconds);
    }
    else {
        spdlog::info("Metrics disabled");
    }

    spdlog::info("Pipeline started - speak into microphone...");
    spdlog::info("Press Ctrl+C to stop");
    if (engine.hasLLM()) spdlog::info("LLM Mode: Your speech will be sent to {} for intelligent responses", llmModel);

#ifdef VASDK_HAS_FTXUI
    {
        // Redirect spdlog so it doesn't corrupt the TUI - append to the TUI log panel
        auto tuiSink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&app](const spdlog::details::log_msg& msg) {
                std::string text(msg.payload.begin(), msg.payload.end());
                app.tuiObserver->appendLog(
                    "[" + std::string(spdlog::level::to_string_view(msg.level).data()) + "] " + text);
            });
        spdlog::default_logger()->sinks().push_back(tuiSink);

        vasdk_app::PipelineTUI tui(app.tuiObserver);
        tui.run(running);
    }
#else
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

void shutdown(va::VAEngine& engine, const MetricsBundle& metrics, [[maybe_unused]] const AppProcessors& app,
              [[maybe_unused]] const va::audio::AudioConfig& inputConfig) {
    spdlog::info("Stopping pipeline...");
    if (metrics.config.collector.enabled) {
        metrics.reporter->printNow();
        metrics.reporter->stop();
    }
#ifdef VASDK_METRICS_PROMETHEUS
    if (metrics.prometheusExporter) metrics.prometheusExporter->stop();
#endif
#ifdef VASDK_METRICS_STATSD
    if (metrics.statsdExporter) metrics.statsdExporter->stop();
#endif
    {
        auto _now = std::chrono::system_clock::now();
        auto _t   = std::chrono::system_clock::to_time_t(_now);
        char _buf[32];
        std::strftime(_buf, sizeof(_buf), "ctrack_%Y%m%d_%H%M%S.txt", std::localtime(&_t));
        VASDK_CTRACK_PRINT_AND_SAVE(_buf);
    }
    engine.stop();
#ifdef VASDK_HAS_JSON
    if (app.sessionWriter) {
        app.sessionWriter->finalize(inputConfig);
        spdlog::info("Session saved to: {}", app.sessionWriter->getSessionDir());
    }
#endif
    spdlog::info("Pipeline stopped");
}

//  Entry point 

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    CliArgs       args    = parseCliArgs(argc, argv);
    loadDotenvFile(args.envFile.empty() ? ".env" : args.envFile);

    // After the env file, so ${VARS} in the config can come from it.
    vasdk::engine::EngineConfig engineConfig;
    if (!args.engineConfigPath.empty()) {
        std::string error;
        if (!va::engine::loadEngineConfig(args.engineConfigPath, engineConfig, &error)) {
            spdlog::error("Engine config: {}", error);
            return -1;
        }
        spdlog::info("Engine config: {}", args.engineConfigPath);
    }

    // Resolve all LLM config once: command line, then engine config, then environment.
    auto orConfig = [](const std::string& cli, const std::string& configured) { return cli.empty() ? configured : cli; };
    args.llmEndpoint = resolveLlmEndpoint(orConfig(args.llmEndpoint, engineConfig.llm().endpoint()));
    args.llmApiKey   = resolveLlmApiKey(orConfig(args.llmApiKey, engineConfig.llm().api_key()));
    args.llmModel    = resolveLlmModel(orConfig(args.llmModel, engineConfig.llm().model()));
    const bool localLlm = !args.llmModelPath.empty();

    args.metricsEnabled = false;
    MetricsBundle metrics = setupMetrics(args);

#ifdef DEBUG
    spdlog::set_level(spdlog::level::debug);
#else
    spdlog::set_level(spdlog::level::info);
#endif
    spdlog::info("VASDK Voice Assistant - Pipeline Mode");
    // An agent with no LLM model named runs on its own: what its pages don't understand gets
    // "Sorry, I can't help with that here." rather than an LLM call.
    const bool agentOnly = !args.agentPath.empty() && !localLlm && args.llmModel.empty();
    if (agentOnly) {
        spdlog::info("  LLM:         none (the agent answers alone; --llm-model adds one for what it doesn't understand)");
        engineConfig.clear_llm();
    } else if (!localLlm) {
        spdlog::info("  LLM endpoint:{}", args.llmEndpoint);
        spdlog::info("  LLM model:   {}", args.llmModel.empty() ? "(not set)" : args.llmModel);
        if (args.llmApiKey.empty()) {
            spdlog::error("No LLM API key found. Set LLM_API_KEY, OPENROUTER_API_KEY, or OPENAI_API_KEY "
                "(in the .env file or shell environment), or pass --llm-api-key <key>");
            return -1;
        }
        spdlog::info("  API key:     {}...{}", args.llmApiKey.substr(0, std::min<size_t>(8, args.llmApiKey.size())),
            args.llmApiKey.size() > 8 ? args.llmApiKey.substr(args.llmApiKey.size() - 4) : "");
        auto* llm = engineConfig.mutable_llm();
        llm->set_endpoint(args.llmEndpoint);
        llm->set_api_key(args.llmApiKey);
        llm->set_model(args.llmModel);
    }
    std::signal(SIGINT, signalHandler);

    std::string modelsPath = findModelsDirectory(argv);
    if (modelsPath.empty()) {
        spdlog::error("Models directory not found (searched up to 3 levels from executable)");
        return -1;
    }

    va::VAEngine::Options options;
    options.modelsDir = modelsPath;
    options.systemPrompt = loadOrFallback(args.systemPromptFile,
        "You are a helpful voice assistant. Keep responses brief and conversational for "
        "text-to-speech. Avoid markdown, bullet points, and emojis. Respond in the user's "
        "language. When you need current data such as weather or time, call the appropriate tool.");
    options.responseFormatInstruction = loadOrFallback(args.formatInstructionFile, kResponseFormatInstruction);
    options.backchannel = args.backchannelConfig;
    options.metricsReporter = metrics.reporter;
    options.metricsConfig = metrics.config;
    {
        std::string audioError;   // the devices open in the config's formats, as the engine will use them
        if (!va::engine::applyAudioConfig(engineConfig.audio(), options.inputAudio, options.outputAudio,
                                          options.frameSize, &audioError)) {
            spdlog::error("{}: {}", args.engineConfigPath, audioError);
            return -1;
        }
    }
    AudioIO io = createAudioIO(args, options.inputAudio);
    options.transport = io.transport;
    if (!args.agentPath.empty()) {
        std::string agentError;
        // The example agent's steps and envelopes written in C++; other agents may use the built-in ones only.
        auto extensions = std::make_shared<va::flow::FlowRegistry>();
        garage::registerExtensions(*extensions);
        options.agent = va::flow::CompiledAgent::load(args.agentPath, &agentError, nullptr, extensions);
        if (!options.agent) {
            spdlog::error("{}: {}", args.agentPath, agentError);
            return -1;
        }
    }
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    options.sendAllFramesToTransport = io.webrtcPeer != nullptr;   // the page shows the agent's state
#endif
#ifdef VASDK_HAS_LLAMACPP
    if (localLlm) options.llmTransport = createLocalLlm(args.llmModelPath, options.systemPrompt);
#endif

    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    std::string error;
    auto engine = va::VAEngine::create(engineConfig, registry, options, &error);
    if (!engine) {
        spdlog::error("{}", error);
        return -1;
    }
    engine->registerTool(WeatherTool{});
    engine->registerTool(TimeTool{});
    AppProcessors app = addAppProcessors(*engine, args, io, modelsPath, options.inputAudio, options.outputAudio);
    addConsoleListener(*engine);
    if (!engine->start(&error)) return -1;

#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (io.webrtcPeer) {
        if (!io.webrtcPeer->startSignalingServer(args.webrtcPort, "127.0.0.1", args.webrtcPage)) return -1;
        spdlog::info("WebRtcTransport ready - open http://127.0.0.1:{} to talk", args.webrtcPort);
        // The page lists the processors and their providers from GET /pipeline.
        io.webrtcPeer->setPipelineDescription([pipeline = std::weak_ptr<va::pipeline::Pipeline>(engine->pipeline())] {
            auto current = pipeline.lock();
            return current ? current->describeAsJson() : std::string("{\"processors\":[]}");
        });
    }
#endif

    runMainLoop(metrics, *engine, args.llmModel, app);

    shutdown(*engine, metrics, app, options.inputAudio);
    return 0;
}
