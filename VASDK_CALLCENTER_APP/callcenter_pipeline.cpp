//  Call Center Voice AI Showcase
//
// A phone call center defined in agent files (agents/callcenter/agent.txtpb):
//   - Routing page : the caller is looked up by their number (ANI) and asked what they need
//   - Forms        : schedule a callback (name, number, service, vehicle for a repair, a future
//                    date, read back and confirmed) or look up an account
//   - CRM tools    : lookup_account and schedule_callback, written in C++ (crm/)
//   - Handoff      : a person on request, or after too many misunderstandings
//   - Recovery     : RecoveryProcessor retries on STT/LLM/TTS errors
//   - Agent per DNIS : --dnis <number> answers with agents/<number>/agent.txtpb when there is one
// It runs with no LLM. With one (--llm-endpoint, or LLM_ENDPOINT in .env), the agent also
// understands what its examples miss, and anything it doesn't cover goes to the LLM.
//
// The call runs as an engine::Session over an engine::Runtime: VAD, STT, TTS and turn detection
// come from the provider catalog, and a server would create one session per call from the same
// runtime, with models loaded once and an agent compiled once per DNIS.
//

#include <VASDK/Pipeline.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/WebRtcTransport.h>
#include <VASDK/RecoveryProcessor.h>
#include <VASDK/FrameProcessor.h>
#include <VASDK/Frame.h>
#include <VASDK/engine/Runtime.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/Expression.h>
#include <VASDK/goal/GoalStatePersistence.h>
#include <VASDK/metrics/CallMetrics.h>

#include <VASDK/llm/Completion.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/OpenAITransport.h>
#include <VASDK/llm/PromptTemplate.h>

#include "CallCenterTools.h"
#include "vasdk/engine/events.pb.h"

// Audio I/O
#ifdef VASDK_HAS_MINIAUDIO
#include "MiniAudio/MiniAudioSource.h"
#include "MiniAudio/MiniAudioSink.h"
#elif defined(VASDK_HAS_SDL)
#include "SDL/SDLAudioSource.h"
#include "SDL/SDLAudioSink.h"
#endif
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
#include "LibDataChannelWebRtc/LibDataChannelWebRtcProvider.h"
#endif

#include "ProviderCatalog/ProviderCatalog.h"

// Speaker tracking / identification (optional)
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
#include <VASDK/SpeakerIdentificationProcessor.h>
#include "PyannoteSpeaker/PyannoteEmbeddingProvider.h"
#endif

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dotenv.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>

//  Call center prompts

static constexpr const char* kCallCenterFormatInstruction =
    "RESPONSE FORMAT - always reply with valid JSON, nothing else:\n"
    "{\"text\": \"<spoken response>\", \"isFinal\": true, \"isExpecting\": true}\n\n"
    "Set \"isFinal\" to false only when you intend to continue speaking before waiting for a reply.\n"
    "Set \"isExpecting\" to false only on your very last utterance (confirmation, farewell, escalation handoff).\n"
    "When the caller must choose from a fixed set of options (e.g. an enum slot), add a \"choices\" array:\n"
    "{\"text\": \"<question>\", \"isFinal\": true, \"isExpecting\": true, "
    "\"choices\": [{\"text\": \"Option A\"}, {\"text\": \"Option B\"}]}\n\n"
    "The \"text\" value is read aloud to the caller - keep it natural and concise.\n"
    "Always put \"text\" first, and open it with a short sentence: it is spoken while the rest is still being written.\n"
    "Never add keys outside this schema or wrap the JSON in markdown.";

// For what the call flow doesn't cover: the agent answers everything it understands itself.
static constexpr const char* kCallCenterSystemPrompt =
    "You are the voice assistant of the Acme call center. The caller said something the call flow "
    "doesn't cover. Answer briefly and politely, in one or two sentences. You can't schedule "
    "callbacks or look up accounts yourself: tell the caller they can say \"schedule a callback\", "
    "\"my account\" or \"talk to a person\".";

//  Globals

std::atomic<bool> running{ true };
std::atomic<bool> sessionDone{ false };    // set when the agent ends the call, or on a fatal error
std::atomic<bool> audioFullyDrained{ false }; // set by TTS drain callback

void signalHandler(int) {
    running = false;
}

//  Helpers 

std::string findModelsDirectory(char* argv[],
    const char* sub = "VASDK_3P_IMPL/models", int maxLevels = 3)
{
    auto path = std::filesystem::canonical(std::filesystem::path(argv[0])).parent_path();
    for (int i = 0; i <= maxLevels; ++i) {
        if (std::filesystem::exists(path / sub)) return (path / sub).string();
        if (!path.has_parent_path() || path == path.parent_path()) break;
        path = path.parent_path();
    }
    return "";
}

//  CLI args 

static std::string loadTextFile(const std::string& path) {
    if (path.empty()) return "";
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("[CallCenter] Could not open prompt file: {}", path);
        return "";
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::string loadOrFallback(const std::string& path, const char* fallback) {
    if (path.empty()) return fallback;
    auto s = loadTextFile(path);
    return s.empty() ? fallback : s;
}

struct CliArgs {
    std::string ani;            ///< Caller phone number (simulates caller ID)
    std::string dnis;           ///< The number called: picks agents/<dnis>/agent.txtpb when there is one
    std::string persistenceDir; ///< Where a call left mid-task is kept, by caller number, for their next call
    std::string callLog;        ///< A file each call's record is appended to, one JSON object per line
    std::string agent;          ///< An agent file to answer with, instead of the DNIS's
    std::string llmEndpoint;
    std::string llmApiKey;
    std::string llmModel;
    std::string llmReasoningEffort;
    std::string systemPromptFile;
    std::string formatInstructionFile;
    std::string envFile = ".env";
    bool        debug = true;
    uint16_t    webrtcPort = 0;     ///< 0 = local mic/speaker
    std::string webrtcPage;
    va::pipeline::BackchannelConfig backchannelConfig;  ///< speech over the agent: back-channel vs barge-in
};

// CLI arg → env var fallback helpers (env vars are populated from .env by loadDotEnv())

static std::string resolveApiKey(const std::string& fromCli) {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_API_KEY"))        return v;
    if (const char* v = std::getenv("OPENROUTER_API_KEY")) return v;
    if (const char* v = std::getenv("OPENAI_API_KEY"))     return v;
    return "";
}

static std::string resolveEndpoint(const std::string& fromCli) {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_ENDPOINT"))   return v;
    if (const char* v = std::getenv("OPENAI_BASE_URL")) return v;
    return "";   // no LLM: the agent answers on its own
}

static std::string resolveModel(const std::string& fromCli) {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_MODEL")) return v;
    return "";
}

static std::string resolveReasoningEffort(const std::string& fromCli) {
    if (!fromCli.empty()) return fromCli;
    if (const char* v = std::getenv("LLM_REASONING_EFFORT")) return v;
    return "";
}

CliArgs parseCliArgs(int argc, char* argv[]) {
    CliArgs args;
    CLI::App app("VASDK Call Center - a call flow in agent files");
    app.allow_extras();
    app.add_option("--ani", args.ani, "Caller ANI (phone number): the agent looks the caller up in the CRM");
    app.add_option("--dnis", args.dnis, "The number called: answered with agents/<dnis>/agent.txtpb when there is one");
    app.add_option("--persist-dir", args.persistenceDir,
        "Directory where a call left mid-form is kept, by the caller's number (--ani): their next call picks it up");
    app.add_option("--call-log", args.callLog,
        "File each call's record is appended to as a JSON line: handle time, completed, escalated, resumed, turns, reprompts");
    app.add_option("--agent", args.agent, "An agent file to answer with (default: agents/callcenter/agent.txtpb)");
    app.add_option("--llm-endpoint", args.llmEndpoint, "OpenAI-compatible LLM endpoint URL");
    app.add_option("--llm-api-key", args.llmApiKey, "LLM API key (falls back to LLM_API_KEY / OPENROUTER_API_KEY / OPENAI_API_KEY env vars)");
    app.add_option("--llm-model", args.llmModel, "LLM model name (falls back to LLM_MODEL env var)");
    app.add_option("--llm-reasoning-effort", args.llmReasoningEffort,
        "reasoning_effort sent to the LLM, e.g. none for a thinking model (falls back to LLM_REASONING_EFFORT env var)");
    app.add_option("--system-prompt", args.systemPromptFile,
        "Path to a text file with the LLM's instructions for what the agent doesn't cover (overrides built-in default)");
    app.add_option("--format-instruction", args.formatInstructionFile,
        "Path to a text file containing the response format instruction (overrides built-in default)");
    app.add_option("--env-file", args.envFile, "Path to .env file (default: .env)");
    app.add_flag("--debug", args.debug, "Enable debug logging");
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
        "Dismissed back-channels: drop, or append them to the caller's next message (default: drop)")
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
        "Take the call over WebRTC instead of the local mic/speaker: serve signaling + demo page on "
        "http://127.0.0.1:<port>");
    app.add_option("--webrtc-page", args.webrtcPage, "Demo page served at / in WebRTC mode")
        ->default_val(VASDK_WEBRTC_DEMO_PAGE);
#endif
    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e) {
        std::exit(app.exit(e));
    }
    return args;
}

//  The agent

// One call's record (call metrics), and where to pick it up when the caller hangs up mid-task.
// Events come on the session's event thread.
struct CallTracker {
    std::mutex mutex;
    va::metrics::CallMetrics metrics;
    std::string lastTask;   // the last snapshot taken in a form, outside the handoff
    std::string lastFlow, lastPage;
    bool agentEnded = false;

    void onEvent(const vasdk::engine::Event& event, const va::engine::Session& call) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.has_agent_turn()) {
            const auto& turn = event.agent_turn();
            ++metrics.turnCount;
            if (turn.event() == "no-match" || turn.event() == "no-input" || turn.event() == "max-reprompts") ++metrics.repromptCount;
            if (!turn.intent().empty() && metrics.intentClassified.empty() && turn.intent() != "help")
                metrics.intentClassified = turn.intent();
            lastFlow = turn.flow();
            lastPage = turn.page();
            const auto snapshot = call.conversationSnapshot();
            if (turn.flow() != "handoff" && snapshot.find("\"form\"") != std::string::npos) {
                lastTask = snapshot;
                metrics.goalName = nlohmann::json::parse(snapshot, nullptr, false).value("/form/goal"_json_pointer, std::string());
            } else if (turn.flow() != "handoff") {
                lastTask.clear();   // out of the form: done with it, or never in one
            }
        }
        if (event.has_page_changed() && event.page_changed().to_flow() == "handoff") metrics.escalated = true;
        if (event.has_session_ended()) agentEnded = true;
    }

    // The caller's number, all but its last 4 digits hidden.
    static std::string masked(const std::string& number) {
        return number.size() <= 4 ? std::string(number.size(), '*') : std::string(number.size() - 4, '*') + number.substr(number.size() - 4);
    }

    nlohmann::json record() {
        std::lock_guard<std::mutex> lock(mutex);
        metrics.goalCompleted = agentEnded && !metrics.escalated;
        return {{"call_id", metrics.callId}, {"ani", masked(metrics.callerANI)}, {"dnis", metrics.calledDNIS},
                {"handle_time_s", metrics.handleTime().count()}, {"completed", metrics.goalCompleted},
                {"escalated", metrics.escalated}, {"resumed", metrics.resumed}, {"intent", metrics.intentClassified},
                {"goal", metrics.goalName}, {"turns", metrics.turnCount}, {"reprompts", metrics.repromptCount},
                {"ended_on", lastFlow + "/" + lastPage}, {"left_mid_task", !agentEnded && !lastTask.empty()}};
    }
};

// The agent for the number called: agents/<dnis>/agent.txtpb when there is one, else the default.
std::string agentFor(const CliArgs& args) {
    if (!args.agent.empty()) return args.agent;
    const std::filesystem::path agents = CALLCENTER_AGENTS_DIR;
    if (!args.dnis.empty() && std::filesystem::exists(agents / args.dnis / "agent.txtpb"))
        return (agents / args.dnis / "agent.txtpb").string();
    return (agents / "callcenter" / "agent.txtpb").string();
}

//  Audio I/O and the call's transport

struct AudioIO {
#ifdef VASDK_HAS_MINIAUDIO
    std::shared_ptr<va::thirdparty::audio::MiniAudioSource> audioSource;
    std::shared_ptr<va::thirdparty::audio::MiniAudioSink>   audioSink;
#elif defined(VASDK_HAS_SDL)
    std::shared_ptr<va::thirdparty::audio::SDLAudioSource> audioSource;
    std::shared_ptr<va::thirdparty::audio::SDLAudioSink>   audioSink;
#endif
    std::shared_ptr<va::transport::AudioTransport> transport;    // LocalTransport or WebRtcTransport
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    std::shared_ptr<va::thirdparty::transport::LibDataChannelWebRtcProvider> webrtcPeer;
#endif
};

// Observes SynthesisStateFrame downstream to drive the audioFullyDrained flag.
// STARTED resets the flag (a new phrase is beginning, previous drain no longer counts).
// COMPLETED sets it (the last audio has fully played out through the speaker).
class SynthesisObserver : public va::pipeline::PassThroughProcessor {
    public:
    std::string getName() const override { return "SynthesisObserver"; }
    void processFrame(std::shared_ptr<va::transport::Frame> frame,
        va::pipeline::FlowDirection dir) override {
        if (dir == va::pipeline::FlowDirection::DOWNSTREAM &&
            frame->getType() == va::transport::FrameType::SYNTHESIS_STATE) {
            auto ssf = std::dynamic_pointer_cast<va::transport::SynthesisStateFrame>(frame);
            if (ssf) {
                if (ssf->state == va::transport::SynthesisState::STARTED)
                    audioFullyDrained = false;
                else if (ssf->state == va::transport::SynthesisState::COMPLETED)
                    audioFullyDrained = true;
            }
        }
        va::pipeline::PassThroughProcessor::processFrame(frame, dir);
    }
};

//  Setup functions

AudioIO createAudioIO(const va::audio::AudioConfig& input, [[maybe_unused]] const CliArgs& args) {
    AudioIO io;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (args.webrtcPort != 0) {
        io.webrtcPeer = std::make_shared<va::thirdparty::transport::LibDataChannelWebRtcProvider>();
        io.transport = std::make_shared<va::transport::WebRtcTransport>(io.webrtcPeer);
        return io;
    }
#endif
#ifdef VASDK_HAS_MINIAUDIO
    io.audioSink = std::make_shared<va::thirdparty::audio::MiniAudioSink>();
    io.audioSource = std::make_shared<va::thirdparty::audio::MiniAudioSource>();
#elif defined(VASDK_HAS_SDL)
    (void)input;
    io.audioSink = std::make_shared<va::thirdparty::audio::SDLAudioSink>();
    io.audioSource = std::make_shared<va::thirdparty::audio::SDLAudioSource>();
#else
#error "No audio I/O backend - enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL"
#endif
    io.transport = std::make_shared<va::transport::LocalTransport>(io.audioSource, io.audioSink);
    return io;
}

//  LLM (optional)

va::llm::LLMConfig llmConfig(const CliArgs& args) {
    va::llm::LLMConfig config;
    config.endpoint = args.llmEndpoint;
    config.apiKey = args.llmApiKey;
    config.model = args.llmModel;
    config.reasoningEffort = args.llmReasoningEffort;
    if (const char* extra = std::getenv("LLM_EXTRA_PAYLOAD")) config.extraPayload = extra;
    config.temperature = 0.15f;
    config.maxTokens = 512 * 8;
    config.timeoutMs = 15000;
    return config;
}

// The call's LLM conversation, for turns the agent doesn't understand.
std::shared_ptr<va::llm::LLMTransport> createLlmTransport(const CliArgs& args) {
    auto config = llmConfig(args);
    config.streaming = true;
    config.systemPrompt = loadOrFallback(args.systemPromptFile, kCallCenterSystemPrompt);
    auto transport = std::make_shared<va::llm::OpenAITransport>(config);
    transport->setPromptChatFormat(va::llm::ChatFormat::OPENAI);
    auto prompt = va::llm::PromptTemplate::create(va::llm::ChatFormat::OPENAI);
    prompt->addSystem(config.systemPrompt);
    transport->setPromptTemplate(std::move(prompt));
    transport->setResponseFormatInstruction(loadOrFallback(args.formatInstructionFile, kCallCenterFormatInstruction));
    transport->onConnected = [] { spdlog::info("[LLM] Connected"); };
    return transport;
}

bool loadDotenvFile(const std::string& path) {
    std::filesystem::path envPath = path;
    bool exists = std::filesystem::exists(envPath);
    std::string absPath = std::filesystem::weakly_canonical(std::filesystem::absolute(envPath)).string();
    if (exists) {
        dotenv::init(path.c_str());
        spdlog::info("  Env file:    {} (loaded)", absPath);
        return true;
    }
    else {
        spdlog::warn("  Env file:    {} (not found - skipped)", absPath);
        return false;
    }
}

//  Main 

int main(int argc, char* argv[]) {
    CliArgs args = parseCliArgs(argc, argv);

    loadDotenvFile(args.envFile.empty() ? ".env" : args.envFile);

    // Resolve LLM config once, after the env file loads, so downstream
    // setup functions just read args directly.
    args.llmEndpoint = resolveEndpoint(args.llmEndpoint);
    args.llmApiKey = resolveApiKey(args.llmApiKey);
    args.llmModel = resolveModel(args.llmModel);
    args.llmReasoningEffort = resolveReasoningEffort(args.llmReasoningEffort);
    const bool online = !args.llmEndpoint.empty();

    spdlog::set_level(args.debug ? spdlog::level::debug : spdlog::level::info);
    spdlog::info("VASDK Call Center - a call flow in agent files");
    spdlog::info("  ANI:         {}", args.ani.empty() ? "(not set)" : args.ani);
    spdlog::info("  DNIS:        {}", args.dnis.empty() ? "(not set)" : args.dnis);
    if (online) {
        spdlog::info("  LLM:         {} ({})", args.llmEndpoint, args.llmModel.empty() ? "default model" : args.llmModel);
        if (!args.llmReasoningEffort.empty()) spdlog::info("  Reasoning:   {}", args.llmReasoningEffort);
    } else {
        spdlog::info("  LLM:         none: the agent answers on its own");
    }

    std::signal(SIGINT, signalHandler);

    std::string modelsPath = findModelsDirectory(argv);
    if (modelsPath.empty()) {
        spdlog::error("[CallCenter] Models directory not found");
        return -1;
    }

    // The agent for the number called, with its CRM tools. A server compiles one per DNIS and
    // shares it between the calls to that number.
    const std::string agentPath = agentFor(args);
    auto tools = std::make_shared<va::flow::FlowRegistry>();
    callcenter::registerTools(*tools);
    std::string error;
    std::vector<std::string> warnings;
    auto agent = va::flow::CompiledAgent::load(agentPath, &error, &warnings, tools);
    for (const auto& warning : warnings) spdlog::warn("[CallCenter] {}: {}", agentPath, warning);
    if (!agent) {
        spdlog::error("[CallCenter] {}: {}", agentPath, error);
        return -1;
    }
    spdlog::info("  Agent:       {}", agentPath);

    // Phone-call turn-taking, and streaming STT for speculative firing.
    // Providers: the first one built in for each kind.
    vasdk::engine::EngineConfig engineConfig;
    engineConfig.mutable_turn_taking()->set_preset(vasdk::engine::TurnTaking::SNAPPY);
    engineConfig.mutable_turn_taking()->set_streaming_transcription(true);
    auto* llmSettings = engineConfig.mutable_llm();
    llmSettings->set_history(12);
    llmSettings->set_speculative_firing(true);   // held until the final transcript matches
    llmSettings->set_speculative_min_words(3);

    va::engine::Runtime::Options runtimeOptions;
    runtimeOptions.modelsDir = modelsPath;
    runtimeOptions.frameSize = 16000 / 8;   // 125 ms at 16 kHz
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    auto runtime = va::engine::Runtime::create(engineConfig, registry, runtimeOptions, &error);
    if (!runtime) {
        spdlog::error("[CallCenter] {}", error);
        return -1;
    }

    const std::string callId = "call-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    AudioIO io = createAudioIO(runtimeOptions.inputAudio, args);
    va::engine::Session::Options callOptions;
    callOptions.id = callId;
    callOptions.transport = io.transport;
    callOptions.agent = agent;
    if (online) {
        callOptions.llmTransport = createLlmTransport(args);
        auto assist = llmConfig(args);
        assist.temperature = 0.0f;
        assist.maxTokens = 400;
        assist.timeoutMs = 8000;
        callOptions.llmCompletion = std::make_shared<va::llm::OpenAICompletion>(assist);
    }
    callOptions.backchannel = args.backchannelConfig;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    callOptions.sendAllFramesToTransport = io.webrtcPeer != nullptr;   // the page shows the agent's state
#endif
    auto call = runtime->createSession(callOptions, &error);
    if (!call) {
        spdlog::error("[CallCenter] {}", error);
        return -1;
    }
    if (!args.ani.empty()) call->setSessionParam("caller", va::flow::stringValue(args.ani));

    // Resume on redial: a caller who hung up mid-form picks up where they were, by their number.
    auto tracker = std::make_shared<CallTracker>();
    tracker->metrics.callId = callId;
    tracker->metrics.callerANI = args.ani;
    tracker->metrics.calledDNIS = args.dnis;
    tracker->metrics.callStart = std::chrono::steady_clock::now();
    std::string callerKey;
    for (const char c : args.ani)
        if (std::isdigit(static_cast<unsigned char>(c))) callerKey += c;
    std::unique_ptr<va::goal::FileGoalStatePersistence> leftCalls;
    if (!args.persistenceDir.empty() && !callerKey.empty()) {
        std::filesystem::create_directories(args.persistenceDir);
        leftCalls = std::make_unique<va::goal::FileGoalStatePersistence>(args.persistenceDir);
        if (const auto left = leftCalls->load(callerKey)) {
            call->resumeConversation(left->dump());
            tracker->metrics.resumed = true;
            spdlog::info("[CallCenter] {} called back: picking up on {}", CallTracker::masked(args.ani), left->value("page", std::string()));
        }
    }

    // The agent ends the call: done, goodbye, or handed to a person.
    call->addListener([tracker, session = call.get()](const vasdk::engine::Event& event) {
        if (event.has_page_changed() && event.page_changed().to_flow() == "handoff")
            spdlog::info("[CallCenter] Transferring the call to a person");
        if (event.has_session_ended()) sessionDone = true;
        tracker->onEvent(event, *session);
    });

    using Slot = va::engine::Session::Slot;
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // After STT: embedding the whole utterance takes ~70 ms per second of speech.
    auto speakerProvider = std::make_shared<va::thirdparty::speaker::PyannoteEmbeddingProvider>(
        va::thirdparty::speaker::PyannoteEmbeddingOptions{modelsPath + "/pyannote_embedding.onnx"});
    if (speakerProvider->initialize(runtimeOptions.inputAudio)) {
        auto speakerId = std::make_shared<va::pipeline::SpeakerIdentificationProcessor>(speakerProvider);
        speakerId->setRingBuffer(call->inputRingBuffer());
        call->addProcessor(Slot::AfterTranscription, speakerId);
    }
    else {
        spdlog::warn("[CallCenter] PyannoteEmbeddingProvider init failed - speaker tracking disabled");
    }
#endif
    // Recovery: retries and apologies on STT/LLM/TTS errors, which travel upstream past it.
    va::RecoveryProcessor::Config recCfg;
    recCfg.maxSttRetries = 5;
    recCfg.maxLlmRetries = 5;
    recCfg.maxTtsRetries = 2;
    recCfg.sttRetryPrompt = "I'm sorry, I didn't catch that - could you say that again?";
    recCfg.llmRetryPrompt = "Bear with me one moment…";
    recCfg.fatalPrompt = "I'm having some technical difficulties. Let me connect you with an agent.";
    recCfg.onFatalError = [] { sessionDone = true; };
    call->addProcessor(Slot::AfterTranscription, std::make_shared<va::RecoveryProcessor>(std::move(recCfg)));
    // Drives audioFullyDrained from the TTS lifecycle, so the closing phrase can finish.
    call->addProcessor(Slot::BeforeOutput, std::make_shared<SynthesisObserver>());

    if (!call->start(&error)) return -1;

#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (io.webrtcPeer) {
        if (!io.webrtcPeer->startSignalingServer(args.webrtcPort, "127.0.0.1", args.webrtcPage)) return -1;
        spdlog::info("[CallCenter] WebRTC ready - open http://127.0.0.1:{} to call", args.webrtcPort);
        // The page lists the processors and their providers from GET /pipeline.
        io.webrtcPeer->setPipelineDescription([weakPipeline = std::weak_ptr<va::pipeline::Pipeline>(call->pipeline())] {
            auto pipeline = weakPipeline.lock();
            return pipeline ? pipeline->describeAsJson() : std::string("{\"processors\":[]}");
        });
    }
#endif

    spdlog::info("[CallCenter] Ready - waiting for caller to speak…");
    spdlog::info("[CallCenter] Press Ctrl+C to end the call");

    // Main loop - wait for signal or session completion
    while (running && !sessionDone) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (sessionDone) {
        // Let the closing phrase play out: poll the drain flag (set when the last synthesis
        // completes) with a safety cap instead of a fixed sleep.
        auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!audioFullyDrained &&
            std::chrono::steady_clock::now() < drainDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    spdlog::info("[CallCenter] Stopping pipeline…");
    call->stop();

    // The call's record, and what's kept for the caller's next call.
    tracker->metrics.callEnd = std::chrono::steady_clock::now();
    const auto record = tracker->record();
    spdlog::info("[CallCenter] Call {}", record.dump());
    if (!args.callLog.empty()) {
        std::ofstream log(args.callLog, std::ios::app);
        log << record.dump() << "\n";
    }
    if (leftCalls) {
        std::lock_guard<std::mutex> lock(tracker->mutex);
        if (!tracker->agentEnded && !tracker->lastTask.empty()) {
            leftCalls->save(callerKey, nlohmann::json::parse(tracker->lastTask));
            spdlog::info("[CallCenter] Left mid-task: kept for the caller's next call");
        } else {
            leftCalls->clear(callerKey);
        }
    }
    spdlog::info("[CallCenter] Done");
    return 0;
}
