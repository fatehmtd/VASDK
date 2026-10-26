#include <VASDK/profiling/CTrack.h>
#include <chrono>
#include <ctime>
#include <VASDK/Pipeline.h>
#include <VASDK/AudioRingBuffer.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/OpenAITransport.h>
#include <VASDK/llm/Tool.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <VASDK/llm/PromptTemplate.h>
#include <VASDK/FrameProcessor.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/TransportInputProcessor.h>
#include <VASDK/TransportOutputProcessor.h>
#include <VASDK/TTSFrameProcessor.h>
#include <VASDK/AudioDSPProcessor.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/AgentStateProcessor.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <VASDK/metrics/MetricsConfig.h>
#include <VASDK/metrics/MetricsReporter.h>
#include <VASDK/metrics/MetricsBinder.h>
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

// VAD providers
#ifdef VASDK_HAS_SILERO_VAD
#include "SileroVAD/SileroVAD.h"
#endif

// STT providers
#ifdef VASDK_HAS_GLADIAPP
#include "GladiappTranscription/GladiaPPTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_SONIOXPP_STT
#include "SonioxppTranscription/SonioxppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_DEEPGRAMPP_STT
#include "DeepgramppTranscription/DeepgramppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_CARTESIAPP_STT
#include "CartesiappTranscription/CartesiappTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_FISHAUDIOPP_STT
#include "FishaudioppTranscription/FishaudioppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_GRADIUMPP_STT
#include "GradiumppTranscription/GradiumppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_PARAKEET
#include "Parakeet/ParakeetTranscriptionProvider.hpp"
#endif

#ifdef VASDK_HAS_MOONSHINE_STT
#include "MoonshineSTT/MoonshineSttProvider.hpp"
#endif

#ifdef VASDK_HAS_WHISPERCPP
#include "WhisperCpp/WhisperCppTranscriptionProvider.hpp"
#endif

#ifdef VASDK_HAS_WHISTLE_STT
#include "WhistleSTT/WhistleTranscriptionProvider.hpp"
#endif

// TTS providers
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
#include "DeepgramppVoiceSynthesis/DeepgramppVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_CARTESIAPP_TTS
#include "CartesiappVoiceSynthesis/CartesiappVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_FISHAUDIOPP_TTS
#include "FishaudioppVoiceSynthesis/FishaudioppVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_GRADIUMPP_TTS
#include "GradiumppVoiceSynthesis/GradiumppVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_SONIOXPP_TTS
#include "SonioxppVoiceSynthesis/SonioxppVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_SUPERTONIC_TTS
#include "SupertonicTTS/SupertonicVoiceSynthesisProvider.hpp"
#endif

#ifdef VASDK_HAS_POCKET_TTS
#include "PocketTTS/PocketVoiceSynthesisProvider.hpp"
#endif

#ifdef VASDK_HAS_PHONON_TTS
#include "PhononTTS/PhononVoiceSynthesisProvider.hpp"
#endif

// DSP providers
#ifdef VASDK_HAS_RNNOISE
#include "RNNoise/RNNoiseDSPProvider.h"
#endif

// Smart turn detection
#ifdef VASDK_HAS_SMART_TURN_V3
#include "SmartTurnV3/SmartTurnV3Provider.h"
#endif

#ifdef VASDK_HAS_LLAMACPP
#include "LlamaCpp/LlamaCppTransport.h"
#endif

#ifdef VASDK_HAS_VIZANIM
#include "VizAnim/VizAnimProvider.h"
#include "VizAnim/VizAnimFrameProcessor.h"
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


// Clears the transcription audio buffer on barge-in so echo-contaminated audio
// captured during TTS playback doesn't poison the next final inference.
class BargeInHandler : public va::pipeline::FrameProcessor {
    std::shared_ptr<va::audio::GenericTranscriptionProvider> _transcriptionProvider;
    public:
    explicit BargeInHandler(std::shared_ptr<va::audio::GenericTranscriptionProvider> p)
        : _transcriptionProvider(p) {
    }

    std::string getName() const override { return "BargeInHandler"; }

    bool shouldProcess(std::shared_ptr<va::transport::Frame> frame) const override {
        return frame->getType() == va::transport::FrameType::AGENT_STATE;
    }

    void processFrame(std::shared_ptr<va::transport::Frame> frame, va::pipeline::FlowDirection direction) override {
        if (direction == va::pipeline::FlowDirection::DOWNSTREAM) {
            auto agentFrame = std::dynamic_pointer_cast<va::transport::AgentStateFrame>(frame);
            if (agentFrame && agentFrame->transition == va::transport::AgentStateTransition::BARGE_IN_INTERRUPT) {
                _transcriptionProvider->clearAudioBuffer();
                spdlog::info("[BargeInHandler] Cleared transcription buffer on barge-in");
            }
        }
        pushDownstream(frame);
    }
};

class STTDebugProcessor : public va::pipeline::FrameProcessor {
    public:
    std::string getName() const override { return "STTDebugProcessor"; }

    bool shouldProcess(std::shared_ptr<va::transport::Frame> frame) const override {
        return frame->getType() == va::transport::FrameType::TRANSCRIPTION;
    }

    void processFrame(std::shared_ptr<va::transport::Frame> frame, va::pipeline::FlowDirection direction) override {
        if (direction == va::pipeline::FlowDirection::DOWNSTREAM) {
            auto tf = std::dynamic_pointer_cast<va::transport::TranscriptionFrame>(frame);
            if (tf) {
                printResults(*tf);
            }
        }
        pushDownstream(frame);
    }

    private:
    void logTranscriptionDetails(const va::transport::TranscriptionFrame& tf) const {
        //if(!tf.isFinal) return;
        const auto& r = tf.transcriptionResult;
        const char* kind = tf.isFinal ? "FINAL" : "PARTIAL";

        spdlog::info("--- STT {} -----------------------------------", kind);
        spdlog::info(" Text      : \"{}\"", r.text);
        spdlog::info(" Confidence: {:.1f}%", r.confidence * 100.0f);

        if (r.startTimeMs > 0 || r.endTimeMs > 0)
            spdlog::info(" Timing    : {}ms - {}ms", r.startTimeMs, r.endTimeMs);

        if (!r.languageTags.empty()) {
            std::string langs;
            for (const auto& l : r.languageTags) {
                if (!langs.empty()) langs += ", ";
                langs += l.code + " (" + std::to_string(static_cast<int>(l.confidence * 100)) + "%)";
            }
            spdlog::info(" Languages : {}", langs);
        }

        if (!r.alternatives.empty()) {
            spdlog::info(" Alternatives ({}):", r.alternatives.size());
            for (size_t i = 0; i < r.alternatives.size(); ++i)
                spdlog::info("   [{}] \"{}\"", i + 1, r.alternatives[i]);
        }

        if (!r.words.empty()) {
            spdlog::info(" Words ({}):", r.words.size());
            for (const auto& w : r.words) {
                std::string extra;
                if (!w.languageTags.empty()) extra += "  lang=" + w.languageTags.front().code;
                if (!w.alternatives.empty()) extra += "  alt=\"" + w.alternatives.front() + "\"";
                spdlog::info("   {:>6}ms - {:>6}ms  {:.0f}%  \"{}\"{}",
                    w.startTimeMs, w.endTimeMs,
                    w.confidence * 100.0f,
                    w.text, extra);
            }
        }

        spdlog::info("------------------------------------------");
    }

    void printResults(const va::transport::TranscriptionFrame& tf) const {
        std::string finalStr = tf.transcriptionResult.text;
        if (finalStr.empty()) {
            for (const auto& w : tf.transcriptionResult.words) {
                finalStr += w.text + " ";
            }
        }

        spdlog::info("Transcription result [{}]: \"{}\" (confidence: {:.1f}%)", tf.isFinal ? "FINAL" : "PARTIAL", finalStr, tf.transcriptionResult.confidence * 100.0f);
    }
};

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

//  Data structs 

struct CliArgs {
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

struct AudioBundle {
    va::audio::AudioConfig inputConfig;
    va::audio::AudioConfig outputConfig;
    int                    frameSize = 0;
#ifdef VASDK_HAS_MINIAUDIO
    std::shared_ptr<va::miniaudio::MiniAudioSource> audioSource;
    std::shared_ptr<va::miniaudio::MiniAudioSink>   audioSink;
#elif defined(VASDK_HAS_SDL)
    std::shared_ptr<SDLAudioSource> audioSource;
    std::shared_ptr<SDLAudioSink>   audioSink;
#endif
#ifdef VASDK_HAS_SILERO_VAD
    std::shared_ptr<SileroVAD>                                vadProvider;
#endif
    std::shared_ptr<va::audio::GenericTranscriptionProvider>  transcriptionProvider;
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> ttsProvider;
    // Type-erased setter so setupProcessors never needs to know the concrete sink type.
    std::function<void(std::function<void(const char*, size_t)>)> setPlaybackCallback;
};

struct LLMBundle {
    va::llm::LLMConfig                              config;
    std::shared_ptr<va::llm::LLMTransport>          transport;
    std::shared_ptr<va::llm::LLMProcessor>          processor;
    std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher;
};

struct TransportBundle {
    std::shared_ptr<va::AudioRingBuffer>           sharedRingBuffer;
    std::shared_ptr<va::AudioRingBuffer>           cleanRingBuffer;
    std::shared_ptr<va::transport::LocalTransport> localTransport;
};

struct ProcessorBundle {
    std::shared_ptr<va::pipeline::TransportInputProcessor>            transportInput;
    std::shared_ptr<va::pipeline::TransportOutputProcessor>           transportOutput;
    std::shared_ptr<va::pipeline::AudioDSPProcessor>                  dspProcessor;
    std::shared_ptr<va::pipeline::VADFrameProcessor>                  vadProcessor;
    std::shared_ptr<va::pipeline::TurnDetectionProcessor>             turnDetection;
    std::shared_ptr<va::pipeline::TranscriptionFrameProcessor>        transcription;
    std::shared_ptr<STTDebugProcessor>                                sttDebug;
    std::shared_ptr<va::pipeline::AgentStateProcessor>                agentState;
    std::shared_ptr<BargeInHandler>                                   bargeinHandler;
    std::shared_ptr<va::llm::LLMProcessor>                            llmProcessor;
    std::shared_ptr<va::llm::ToolDispatchProcessor>                   toolDispatcher;
    std::shared_ptr<va::pipeline::TTSFrameProcessor>                  tts;
    std::shared_ptr<va::thirdparty::AudioInputVizAnimFrameProcessor>  micAnimProcessor;
    std::shared_ptr<va::thirdparty::OutputAudioVizAnimFrameProcessor> animProcessor;
#ifdef VASDK_HAS_JSON
    std::shared_ptr<va::pipeline::SessionWriter>   sessionWriter;
    std::shared_ptr<va::pipeline::SessionRecorder> audioRec;
    std::shared_ptr<va::pipeline::SessionRecorder> semRec;
#endif
#ifdef VASDK_HAS_FTXUI
    std::shared_ptr<vasdk_app::AudioFeaturesObserver> tuiObserver;
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

std::optional<AudioBundle> setupAudioProviders(const std::string& modelsPath, int frameSize) {
    AudioBundle b;
    b.frameSize = frameSize;

    b.inputConfig.sampleRate  = 16000;
    b.inputConfig.channels    = 1;
    b.inputConfig.format      = va::audio::AudioFormat::SHORT;
    b.outputConfig.sampleRate = 16000;
    b.outputConfig.channels   = 1;
    b.outputConfig.format     = va::audio::AudioFormat::SHORT;

#ifdef VASDK_HAS_MINIAUDIO
    b.audioSink   = std::make_shared<va::miniaudio::MiniAudioSink>();
    b.audioSource = std::make_shared<va::miniaudio::MiniAudioSource>(
        b.inputConfig.sampleRate * b.inputConfig.channels * b.inputConfig.getSampleSizeInBytes() * 30);
    b.setPlaybackCallback = [sink = b.audioSink](auto cb) { sink->setPlaybackCallback(std::move(cb)); };
#elif defined(VASDK_HAS_SDL)
    b.audioSink   = std::make_shared<SDLAudioSink>();
    b.audioSource = std::make_shared<SDLAudioSource>();
    b.setPlaybackCallback = [sink = b.audioSink](auto cb) { sink->setPlaybackCallback(std::move(cb)); };
#else
#error "No audio I/O backend available. Enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL."
#endif

#ifdef VASDK_HAS_SILERO_VAD
    b.vadProvider = std::make_shared<SileroVAD>(modelsPath + "/silero_vad.onnx");
    if (!b.vadProvider->initialize(b.inputConfig, b.inputConfig.sampleRate * 4)) {
        spdlog::error("Failed to initialize VAD provider");
        return std::nullopt;
    }
#endif

#if defined(VASDK_HAS_GRADIUMPP_STT)
    auto gradiumStt = std::make_shared<va::thirdparty::GradiumppTranscriptionProvider>(
        std::getenv("GRADIUM_API_KEY") ? std::getenv("GRADIUM_API_KEY") : "");
    b.transcriptionProvider = gradiumStt;
#elif defined(VASDK_HAS_SONIOXPP_STT)
    auto soniox = std::make_shared<va::thirdparty::SonioxppTranscriptionProvider>(
        std::getenv("SONIOX_API_KEY") ? std::getenv("SONIOX_API_KEY") : "");
    b.transcriptionProvider = soniox;
#elif defined(VASDK_HAS_DEEPGRAMPP_STT)
    auto deepgramStt = std::make_shared<va::thirdparty::DeepgramppTranscriptionProvider>(
        std::getenv("DEEPGRAM_API_KEY") ? std::getenv("DEEPGRAM_API_KEY") : "");
    b.transcriptionProvider = deepgramStt;
#elif defined(VASDK_HAS_CARTESIAPP_STT)
    auto cartesiaStt = std::make_shared<va::thirdparty::CartesiappTranscriptionProvider>(
        std::getenv("CARTESIA_API_KEY") ? std::getenv("CARTESIA_API_KEY") : "");
    b.transcriptionProvider = cartesiaStt;
#elif defined(VASDK_HAS_FISHAUDIOPP_STT)
    auto fishaudioStt = std::make_shared<va::thirdparty::FishaudioppTranscriptionProvider>(
        std::getenv("FISHAUDIO_API_KEY") ? std::getenv("FISHAUDIO_API_KEY") : "");
    b.transcriptionProvider = fishaudioStt;
#elif defined(VASDK_HAS_WHISTLE_STT)
    // Local/offline: opt-in (VASDK_BUILD_WHISTLE_STT), so it wins over Moonshine/Parakeet when built.
    auto whistle = std::make_shared<WhistleTranscriptionProvider>(modelsPath + "/whistle");
    WhistleTranscriptionProvider::InferenceConfig inferenceConfig;
    inferenceConfig.idleTimeoutMs           = 550;
    inferenceConfig.partialIntervalMs       = 250;
    inferenceConfig.minNewSamplesForPartial = b.inputConfig.sampleRate / 4;
    inferenceConfig.minNewSamplesForFinal   = frameSize;
    if (const char* lang = std::getenv("WHISTLE_LANGUAGE")) inferenceConfig.language = lang;
    whistle->setInferenceConfig(inferenceConfig);
    b.transcriptionProvider = whistle;
#elif defined(VASDK_HAS_MOONSHINE_STT)
    auto moonshine = std::make_shared<MoonshineSttProvider>(modelsPath + "/moonshine");
    MoonshineSttProvider::InferenceConfig inferenceConfig;
    inferenceConfig.idleTimeoutMs           = 350;
    inferenceConfig.partialIntervalMs       = 80;
    inferenceConfig.minNewSamplesForPartial = frameSize;
    inferenceConfig.minNewSamplesForFinal   = frameSize;
    moonshine->setInferenceConfig(inferenceConfig);
    b.transcriptionProvider = moonshine;
#elif defined(VASDK_HAS_PARAKEET)
    auto parakeet = std::make_shared<ParakeetTranscriptionProvider>(modelsPath + "/parakeet");
    ParakeetTranscriptionProvider::InferenceConfig inferenceConfig;
    inferenceConfig.idleTimeoutMs           = 550;
    inferenceConfig.partialIntervalMs       = 250;
    inferenceConfig.minNewSamplesForPartial = b.inputConfig.sampleRate / 2;
    inferenceConfig.minNewSamplesForFinal   = inferenceConfig.minNewSamplesForPartial * 2;
    inferenceConfig.contextSamples          = b.inputConfig.sampleRate / 2;
    inferenceConfig.silencePaddingSeconds   = 1.0f;
    parakeet->setInferenceConfig(inferenceConfig);
    b.transcriptionProvider = parakeet;
#else
#error "No transcription provider available - enable VASDK_HAS_SONIOXPP_STT, VASDK_HAS_DEEPGRAMPP_STT, VASDK_HAS_CARTESIAPP_STT, VASDK_HAS_FISHAUDIOPP_STT, VASDK_HAS_GRADIUMPP_STT, VASDK_HAS_WHISTLE_STT, VASDK_HAS_MOONSHINE_STT, VASDK_HAS_PARAKEET, or VASDK_HAS_WHISPERCPP"
#endif
    b.transcriptionProvider->setAudioConfiguration(b.inputConfig);

#if defined(VASDK_HAS_GRADIUMPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::GradiumppVoiceSynthesisProvider>(
        std::getenv("GRADIUM_API_KEY") ? std::getenv("GRADIUM_API_KEY") : "",
        std::getenv("GRADIUM_VOICE_ID") ? std::getenv("GRADIUM_VOICE_ID") : "");
#elif defined(VASDK_HAS_SONIOXPP_TTS)
    auto sonioxTts = std::make_shared<va::thirdparty::SonioxppVoiceSynthesisProvider>(
        std::getenv("SONIOX_API_KEY") ? std::getenv("SONIOX_API_KEY") : "");
    b.ttsProvider = sonioxTts;
#elif defined(VASDK_HAS_SUPERTONIC_TTS)
    auto supertonic = std::make_shared<SupertonicVoiceSynthesisProvider>(
        modelsPath + "/supertonic/onnx",
        modelsPath + "/supertonic/voice_styles/M1.json");
    supertonic->setTotalSteps(15);
    b.ttsProvider = supertonic;
#elif defined(VASDK_HAS_DEEPGRAMPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::DeepgramppVoiceSynthesisProvider>(
        std::getenv("DEEPGRAM_API_KEY") ? std::getenv("DEEPGRAM_API_KEY") : "");
#elif defined(VASDK_HAS_CARTESIAPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::CartesiappVoiceSynthesisProvider>(
        std::getenv("CARTESIA_API_KEY") ? std::getenv("CARTESIA_API_KEY") : "",
        std::getenv("CARTESIA_VOICE_ID") ? std::getenv("CARTESIA_VOICE_ID") : "");
#elif defined(VASDK_HAS_FISHAUDIOPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::FishaudioppVoiceSynthesisProvider>(
        std::getenv("FISHAUDIO_API_KEY") ? std::getenv("FISHAUDIO_API_KEY") : "",
        std::getenv("FISHAUDIO_VOICE_ID") ? std::getenv("FISHAUDIO_VOICE_ID") : "");
#elif defined(VASDK_HAS_PHONON_TTS)
    // Local/offline: opt-in (VASDK_BUILD_PHONON_TTS), so it wins over PocketTTS when built.
    auto phonon = std::make_shared<PhononVoiceSynthesisProvider>(
        modelsPath + "/phonon_tts",
        std::getenv("PHONON_TTS_VOICE") ? std::getenv("PHONON_TTS_VOICE") : "Easton",
        std::getenv("PHONON_TTS_LANG") ? std::getenv("PHONON_TTS_LANG") : "en");
    if (const char* rules = std::getenv("PHONON_TTS_RULES")) phonon->setNormalizationRules(rules);
    phonon->setSilenceDuration(0.3f);
    b.ttsProvider = phonon;
#elif defined(VASDK_HAS_POCKET_TTS)
    // Local/offline fallback: only used when no cloud TTS provider is compiled in.
    auto pocket = std::make_shared<PocketVoiceSynthesisProvider>(modelsPath + "/pocket_tts", "cosette");
    pocket->setLsdSteps(4);
    pocket->setVoice("jean");
    pocket->setPreBufferedSamplesMS(250);
    pocket->setSilenceDuration(0.3f);
    b.ttsProvider = pocket;
#else
#error "No TTS provider available - enable VASDK_BUILD_SONIOXPP_TTS, VASDK_BUILD_POCKET_TTS, VASDK_BUILD_PHONON_TTS, VASDK_BUILD_SUPERTONIC_TTS, VASDK_BUILD_DEEPGRAMPP_TTS, VASDK_BUILD_CARTESIAPP_TTS, VASDK_BUILD_FISHAUDIOPP_TTS, or VASDK_BUILD_GRADIUMPP_TTS"
#endif
    b.ttsProvider->setAudioConfiguration(b.outputConfig);

    return b;
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

LLMBundle setupLLMStack(const std::string& modelsPath, const std::string& llmModelPath,
                        const std::string& llmEndpoint, const std::string& llmApiKey,
                        const std::string& llmModel,
                        const std::string& systemPrompt,
                        const std::string& formatInstruction) {
    LLMBundle llmBundle;

    llmBundle.config.endpoint = llmEndpoint;
    llmBundle.config.apiKey   = llmApiKey;
    llmBundle.config.model    = llmModel;
    
    llmBundle.config.systemPrompt = systemPrompt;
    llmBundle.config.temperature = 0.1f;
    llmBundle.config.maxTokens   = 8000;
    llmBundle.config.nCtx        = 64000;
    llmBundle.config.timeoutMs   = 30000;
    llmBundle.config.streaming   = true;

    spdlog::info("LLM Config: endpoint={}, model={}, streaming={}, system prompt={}",
        llmBundle.config.endpoint, llmBundle.config.model,
        llmBundle.config.streaming ? "true" : "false", llmBundle.config.systemPrompt);

    llmBundle.toolDispatcher = std::make_shared<va::llm::ToolDispatchProcessor>();
    llmBundle.toolDispatcher->registerTool(WeatherTool{});
    llmBundle.toolDispatcher->registerTool(TimeTool{});

    bool useLocalLLM = false;
#ifdef VASDK_HAS_LLAMACPP
    std::string resolvedModelPath = llmModelPath;
#ifdef VASDK_LLAMACPP_DEFAULT_MODEL
    if (resolvedModelPath.empty() && !modelsPath.empty()) {
        resolvedModelPath = modelsPath + "/" + VASDK_LLAMACPP_DEFAULT_MODEL;
        spdlog::info("[LLM] Using default GGUF model: {}", resolvedModelPath);
    }
#endif
    if (!resolvedModelPath.empty() &&
        std::filesystem::exists(resolvedModelPath) &&
        useLocalLLM) {
        spdlog::info("LLM integration enabled with llama.cpp local transport (model: {})", resolvedModelPath);
        va::llm::LLMConfig localCfg;
        localCfg.model             = resolvedModelPath;
        localCfg.temperature       = llmBundle.config.temperature;
        localCfg.maxTokens         = llmBundle.config.maxTokens;
        localCfg.nCtx              = 16000;
        localCfg.topK              = 50;
        localCfg.repetitionPenalty = 1.05f;
        // LlamaCppTransport reads directly from _conversationHistory, so tool descriptions
        // must be embedded in the system prompt - the PromptTemplate resolver is never called.
        localCfg.systemPrompt = llmBundle.config.systemPrompt + "\n\n"
            + llmBundle.toolDispatcher->renderToolDescriptions(va::llm::ChatFormat::LFM2);
        llmBundle.transport = std::make_shared<va::transport::LlamaCppTransport>(localCfg);
        llmBundle.transport->setResponseFormatInstruction(formatInstruction);
        std::reinterpret_pointer_cast<va::transport::LlamaCppTransport>(llmBundle.transport)
            ->setChunker(std::make_shared<va::transport::NoChunkingChunker>());
    }
    else
#endif
    {
        spdlog::info("LLM integration enabled with OpenAI-compatible transport ({})", llmBundle.config.endpoint);
        spdlog::info("LLM streaming mode: {}", llmBundle.config.streaming ? "enabled" : "disabled");
        llmBundle.transport = std::make_shared<va::llm::OpenAITransport>(llmBundle.config);
        llmBundle.transport->setResponseFormatInstruction(formatInstruction);

        auto sysTemplate = va::llm::PromptTemplate::create(llmBundle.transport->getPromptChatFormat());
        sysTemplate->addSystem(llmBundle.config.systemPrompt + "\n\n{tool_descriptions}");
        llmBundle.transport->setPromptTemplate(std::move(sysTemplate));
        llmBundle.transport->registerTemplateVar("tool_descriptions",
            [td = llmBundle.toolDispatcher](va::llm::ChatFormat fmt) {
                return td->renderToolDescriptions(fmt);
            });
    }

    llmBundle.transport->onConnected = []() { spdlog::info("LLM Transport connected"); };
    llmBundle.transport->onError = [](int code, const std::string& msg) {
        spdlog::error("LLM Error [{}]: {}", code, msg);
    };

    llmBundle.processor = std::make_shared<va::llm::LLMProcessor>(llmBundle.transport);
    llmBundle.processor->setHistoryEnabled(true);
    llmBundle.processor->setMaxHistoryLength(10);
    llmBundle.processor->setProcessPartialTranscriptions(false);
    llmBundle.processor->setConfidenceThreshold(0.07f);
    llmBundle.processor->setFirePolicy(std::make_shared<va::llm::FinalOnlyFirePolicy>());
    llmBundle.processor->setToolDispatcher(llmBundle.toolDispatcher);

    return llmBundle;
}

std::optional<TransportBundle> setupTransport(const AudioBundle& audio, const MetricsBundle& metrics) {
    TransportBundle b;
    b.sharedRingBuffer = std::make_shared<va::AudioRingBuffer>(
        (long)audio.inputConfig.sampleRate * 90, audio.inputConfig.getSampleSizeInBytes());
    b.cleanRingBuffer = std::make_shared<va::AudioRingBuffer>(
        (long)audio.inputConfig.sampleRate * 90, audio.inputConfig.getSampleSizeInBytes());

    b.localTransport = std::make_shared<va::transport::LocalTransport>(audio.audioSource, audio.audioSink);
    b.localTransport->setInputAudioConfig(audio.inputConfig);
    b.localTransport->setOutputAudioConfig(audio.outputConfig);
    b.localTransport->setFrameSize(audio.frameSize);
    b.localTransport->setRingBuffer(b.sharedRingBuffer);
    b.localTransport->setMetricsConfig(metrics.config.transport);

    if (!b.localTransport->initialize()) {
        spdlog::error("Failed to initialize LocalTransport");
        return std::nullopt;
    }
    if (!b.localTransport->start()) {
        spdlog::error("Failed to start LocalTransport");
        return std::nullopt;
    }
    spdlog::info("LocalTransport started successfully");

    metrics.reporter->addTransport(b.localTransport);
    return b;
}

std::optional<ProcessorBundle> setupProcessors(
    const std::string&     modelsPath,
    const AudioBundle&     audio,
    const TransportBundle& transport,
    const LLMBundle&       llm,
    const std::string&     sessionOutputDir)
{
    ProcessorBundle b;

    b.transportInput = std::make_shared<va::pipeline::TransportInputProcessor>();
    b.transportInput->addTransport(transport.localTransport);

    b.transportOutput = std::make_shared<va::pipeline::TransportOutputProcessor>();
    b.transportOutput->addTransport(transport.localTransport, va::transport::FrameType::SYNTHESIS);

    //  RNNoise noise-suppression (optional, compile-time) 
    // Set useRNNoise=true at runtime to enable noise suppression without recompiling.
#ifdef VASDK_HAS_RNNOISE
    bool useRNNoise = false;
    if (useRNNoise) {
        auto rnnoiseProvider = std::make_shared<RNNoiseDSPProvider>();
        rnnoiseProvider->setVADThreshold(0.3f);
        rnnoiseProvider->setNormalizeOutput(false);
        rnnoiseProvider->setNormalizationMultiplier(0.3f);
        if (!rnnoiseProvider->initialize(audio.inputConfig)) {
            spdlog::warn("RNNoise initialization failed - noise suppression disabled");
        }
        else {
            b.dspProcessor = std::make_shared<va::pipeline::AudioDSPProcessor>(rnnoiseProvider);
            b.dspProcessor->setRawAudioRingBuffer(transport.sharedRingBuffer);
            b.dspProcessor->setCleanAudioRingBuffer(transport.cleanRingBuffer);
            spdlog::info("RNNoise noise suppression enabled");
        }
    }
#endif

    auto audioRingBufferForVAD = b.dspProcessor ? transport.cleanRingBuffer : transport.sharedRingBuffer;

    float vadLoopBackFrameMultiplier = 2.0f;
    b.vadProcessor = std::make_shared<va::pipeline::VADFrameProcessor>(audio.vadProvider, vadLoopBackFrameMultiplier);
    b.vadProcessor->setMinFrameSize(audio.frameSize);

    va::pipeline::VADConfig vadConfig;
    vadConfig.vadThresholdMs           = 250;
    vadConfig.minSpeechDurationMs      = 250;
    vadConfig.maxSpeechPauseDurationMs = 500;
    vadConfig.maxSpeechEndDurationMs   = 250;
    b.vadProcessor->setConfig(vadConfig);
    b.vadProcessor->setRingBuffer(audioRingBufferForVAD);

#ifdef VASDK_HAS_SMART_TURN_V3
    auto smartTurnProvider = std::make_shared<va::thirdparty::SmartTurnV3Provider>(
        modelsPath + "/smart_turn_v3.onnx", 0.5f, 8.0f, 3);
    if (!smartTurnProvider->initialize(audio.inputConfig, transport.sharedRingBuffer)) {
        spdlog::error("Failed to initialize SmartTurnV3Provider - turn detection disabled");
        return std::nullopt;
    }
    b.turnDetection = std::make_shared<va::pipeline::TurnDetectionProcessor>(smartTurnProvider);
#else
    b.turnDetection = std::make_shared<va::pipeline::TurnDetectionProcessor>();
#endif

    b.agentState = std::make_shared<va::pipeline::AgentStateProcessor>();
    b.agentState->setAlwaysOn(true);

    b.transcription = std::make_shared<va::pipeline::TranscriptionFrameProcessor>(audio.transcriptionProvider);
    b.transcription->configSilenceBehavior(false, false, 1000);
    b.transcription->setStreamingMode(false);
    b.transcription->setRingBuffer(audioRingBufferForVAD);

    b.sttDebug       = std::make_shared<STTDebugProcessor>();
    b.bargeinHandler = std::make_shared<BargeInHandler>(audio.transcriptionProvider);

    b.tts = std::make_shared<va::pipeline::TTSFrameProcessor>(audio.ttsProvider);
    b.tts->setPlaybackFlush([lt = transport.localTransport]() { lt->flushOutputAudio(); });

    b.llmProcessor   = llm.processor;
    b.toolDispatcher = llm.toolDispatcher;

#ifdef VASDK_HAS_JSON
    if (!sessionOutputDir.empty()) {
        b.sessionWriter = std::make_shared<va::pipeline::SessionWriter>(sessionOutputDir);
        b.audioRec = std::make_shared<va::pipeline::SessionRecorder>(b.sessionWriter, "AudioRecorder");
        b.audioRec->setRingBuffer(audioRingBufferForVAD);
        b.semRec = std::make_shared<va::pipeline::SessionRecorder>(b.sessionWriter, "SemanticRecorder");
        spdlog::info("Session recording enabled → {}", b.sessionWriter->getSessionDir());
    }
#endif

    b.micAnimProcessor = std::make_shared<va::thirdparty::AudioInputVizAnimFrameProcessor>(
        audioRingBufferForVAD, audio.inputConfig, audio.inputConfig.sampleRate * 10);
    b.animProcessor = std::make_shared<va::thirdparty::OutputAudioVizAnimFrameProcessor>(
        audio.inputConfig, audio.inputConfig.sampleRate * 10);

#ifdef VASDK_HAS_FTXUI
    b.tuiObserver = std::make_shared<vasdk_app::AudioFeaturesObserver>();
    b.tuiObserver->setSynthAudioConfig(audio.outputConfig);
    if (audio.setPlaybackCallback) {
        audio.setPlaybackCallback(
            [obs = std::weak_ptr<vasdk_app::AudioFeaturesObserver>(b.tuiObserver)](
                    const char* data, size_t bytes) {
                if (auto o = obs.lock()) o->onSynthPlayback(data, bytes);
            });
    }
#endif

    return b;
}

std::shared_ptr<va::pipeline::Pipeline> buildAndStartPipeline(
    const ProcessorBundle& procs,
    const MetricsBundle&   metrics)
{
    auto pipeline = std::make_shared<va::pipeline::Pipeline>();
    auto& chain   = pipeline->addProcessor(procs.transportInput);
#ifdef VASDK_HAS_RNNOISE
    if (procs.dspProcessor) chain.addProcessor(procs.dspProcessor);
#endif
#ifdef VASDK_HAS_JSON
    if (procs.audioRec) chain.addProcessor(procs.audioRec);
#endif
    chain
        .addProcessor(procs.vadProcessor)
        .addProcessor(procs.turnDetection)
        .addProcessor(procs.micAnimProcessor)
        .addProcessor(procs.transcription)
        .addProcessor(procs.sttDebug)
        .addProcessor(procs.agentState)
        .addProcessor(procs.bargeinHandler);
    if (procs.llmProcessor)   chain.addProcessor(procs.llmProcessor);
    if (procs.toolDispatcher) chain.addProcessor(procs.toolDispatcher);
#ifdef VASDK_HAS_JSON
    if (procs.semRec) chain.addProcessor(procs.semRec);
#endif
    chain
        .addProcessor(procs.tts)
        .addProcessor(procs.animProcessor)
#ifdef VASDK_HAS_FTXUI
        .addProcessor(procs.tuiObserver)
#endif
        .addProcessor(procs.transportOutput);

    spdlog::info("Building pipeline with {} processors...", chain.getProcessors().size());

    // Metrics wiring - after processors added, before build()
    va::metrics::MetricsBinder::bind(*pipeline, metrics.config, metrics.reporter);

    if (!pipeline->build()) {
        spdlog::error("Failed to build pipeline");
        return nullptr;
    }
    spdlog::info("Pipeline built successfully");

    spdlog::info("Starting pipeline...");
    if (!pipeline->start()) {
        spdlog::error("Failed to start pipeline");
        return nullptr;
    }
    return pipeline;
}

void runMainLoop(
    const MetricsBundle&   metrics,
    const LLMBundle&       llm,
    const ProcessorBundle& procs)
{
    // Start reporter after pipeline is running so all metrics are registered
    // and the first periodic tick has real data (default interval: 10s).
    if (metrics.config.collector.enabled) {
        metrics.reporter->start();
        spdlog::info("Metrics active - reporting every {}s",
            metrics.config.reporter.intervalSeconds);
    }
    else {
        spdlog::info("Metrics disabled");
    }

    spdlog::info("Pipeline started - speak into microphone...");
    spdlog::info("Press Ctrl+C to stop");

    if (llm.processor) {
        spdlog::info("LLM Mode: Your speech will be sent to {} for intelligent responses",
            llm.config.model);
    }
    else {
        spdlog::info("Echo Mode: Transcriptions will be played back via TTS (no LLM)");
    }

#ifdef VASDK_HAS_FTXUI
    {
        // Redirect spdlog so it doesn't corrupt the TUI - append to the TUI log panel
        auto tuiSink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&procs](const spdlog::details::log_msg& msg) {
                std::string text(msg.payload.begin(), msg.payload.end());
                procs.tuiObserver->appendLog(
                    "[" + std::string(spdlog::level::to_string_view(msg.level).data()) + "] " + text);
            });
        spdlog::default_logger()->sinks().push_back(tuiSink);

        vasdk_app::PipelineTUI tui(procs.tuiObserver);
        tui.run(running);
    }
#else
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

void shutdown(
    std::shared_ptr<va::pipeline::Pipeline> pipeline,
    const MetricsBundle&                    metrics,
    const ProcessorBundle&                  procs,
    const AudioBundle&                      audio)
{
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
    pipeline->stop();
#ifdef VASDK_HAS_JSON
    if (procs.sessionWriter) {
        procs.sessionWriter->finalize(audio.inputConfig);
        spdlog::info("Session saved to: {}", procs.sessionWriter->getSessionDir());
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

    // Resolve all LLM config once after the env file is loaded.
    args.llmEndpoint = resolveLlmEndpoint(args.llmEndpoint);
    args.llmApiKey   = resolveLlmApiKey(args.llmApiKey);
    args.llmModel    = resolveLlmModel(args.llmModel);

    args.metricsEnabled = false;
    MetricsBundle metrics = setupMetrics(args);

#ifdef DEBUG
    spdlog::set_level(spdlog::level::debug);
#else
    spdlog::set_level(spdlog::level::info);
#endif
    spdlog::info("VASDK Voice Assistant - Pipeline Mode");
    spdlog::info("  LLM endpoint:{}", args.llmEndpoint);
    spdlog::info("  LLM model:   {}", args.llmModel.empty() ? "(not set)" : args.llmModel);
    if (args.llmApiKey.empty()) {
        spdlog::error("No LLM API key found. Set LLM_API_KEY, OPENROUTER_API_KEY, or OPENAI_API_KEY "
            "(in the .env file or shell environment), or pass --llm-api-key <key>");
        return -1;
    }
    spdlog::info("  API key:     {}...{}", args.llmApiKey.substr(0, std::min<size_t>(8, args.llmApiKey.size())),
        args.llmApiKey.size() > 8 ? args.llmApiKey.substr(args.llmApiKey.size() - 4) : "");
    std::signal(SIGINT, signalHandler);

    std::string modelsPath = findModelsDirectory(argv);
    if (modelsPath.empty()) {
        spdlog::error("Models directory not found (searched up to 3 levels from executable)");
        return -1;
    }

    int frameSize = 16000 / 4;  // 125ms at 16kHz
    auto audioOpt = setupAudioProviders(modelsPath, frameSize);
    if (!audioOpt) return -1;

    const std::string systemPrompt = loadOrFallback(args.systemPromptFile,
        "You are a helpful voice assistant. Keep responses brief and conversational for "
        "text-to-speech. Avoid markdown, bullet points, and emojis. Respond in the user's "
        "language. When you need current data such as weather or time, call the appropriate tool.");
    const std::string formatInstruction = loadOrFallback(args.formatInstructionFile,
        kResponseFormatInstruction);

    LLMBundle llm = setupLLMStack(modelsPath, args.llmModelPath,
                                  args.llmEndpoint, args.llmApiKey, args.llmModel,
                                  systemPrompt, formatInstruction);

    auto transportOpt = setupTransport(*audioOpt, metrics);
    if (!transportOpt) return -1;

    auto procsOpt = setupProcessors(modelsPath, *audioOpt, *transportOpt, llm, args.sessionOutputDir);
    if (!procsOpt) return -1;

    auto pipeline = buildAndStartPipeline(*procsOpt, metrics);
    if (!pipeline) return -1;

    runMainLoop(metrics, llm, *procsOpt);

    shutdown(pipeline, metrics, *procsOpt, *audioOpt);
    return 0;
}
