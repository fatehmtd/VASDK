//  Call Center Voice AI Showcase 
//
// Demonstrates the goal-oriented call center pipeline built on VASDK:
//   - IntentRouter  : classifies caller intent from the first utterance
//   - GoalManager   : drives multi-turn slot filling toward a completion action
//   - Slot types    : STRING, PHONE, DATE, ENUM, OBJECT, COMPOSITE, conditional
//   - Confirmation  : request_confirmation recap before firing the action
//   - Escalation    : escalate_to_human on explicit request or max reprompts
//   - Recovery      : RecoveryProcessor retries on STT/LLM/TTS errors
//   - Persistence   : FileGoalStatePersistence resumes partial sessions on redial
//   - Pre-fill      : caller ANI → simulated CRM lookup → pre-filled slots
//   - CRM tool      : LLM can call lookup_account() mid-conversation
//
// The call runs as an engine::Session over an engine::Runtime: VAD, STT, TTS and turn detection
// come from the provider catalog, and a server would create one session per call from the same
// runtime, with models loaded once.
// 

#include <VASDK/Pipeline.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/WebRtcTransport.h>
#include <VASDK/RecoveryProcessor.h>
#include <VASDK/FrameProcessor.h>
#include <VASDK/Frame.h>
#include <VASDK/engine/Runtime.h>

#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/OpenAITransport.h>
#include <VASDK/llm/LLMFirePolicy.h>
#include <VASDK/llm/Tool.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <VASDK/llm/PromptTemplate.h>

#include <VASDK/goal/GoalSlot.h>
#include <VASDK/goal/GoalDefinition.h>
#include <VASDK/goal/GoalState.h>
#include <VASDK/goal/GoalManager.h>
#include <VASDK/goal/GoalStatePersistence.h>
#include <VASDK/goal/IntentRouter.h>

#include <VASDK/metrics/CallMetrics.h>

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
#include <filesystem>
#include <fstream>
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

static constexpr const char* kCallCenterSystemPromptTemplate = R"(
You are a helpful call center assistant.

Instructions:
- Collect missing information through natural conversation; do not recite the lists below.
- The moment the caller provides any required scalar value (name, phone, date, enum choice),
  call update_slot() immediately - do not wait for the turn to end.
- For OBJECT-type slots (labelled "use update_object_slot"), call update_object_slot() and
  pass all known fields as a JSON object.
- Use dot notation for COMPOSITE sub-fields (e.g. "address.city").
- If the caller provides several pieces of information in one sentence, call update_slot()
  once per field before generating your spoken response.
- If a slot value is rejected (tool returns success=false), re-prompt naturally using the
  error message - do not repeat the raw error to the caller verbatim.
- If the caller goes off-topic, answer briefly then redirect back to the goal.
- If the caller tries to end the call before all required information is collected, acknowledge
  their intent and ask one focused question to continue making progress.
- When ALL required slots are filled, call request_confirmation() with a natural recap
  before calling complete_goal().
- If the caller confirms the recap, call complete_goal().
- If the caller corrects any detail, call update_slot/update_object_slot to fix it,
  then call request_confirmation() again.
- If the caller explicitly requests a human agent, or if the situation is outside your
  scope, call escalate() immediately.

Available tools:
{tool_descriptions}

Your goal: {goal_objective}

Information already collected:
{collected_slots}

Current status: {goal_status}
{escalation_hint}
)";

//  Globals

std::atomic<bool> running{ true };
std::atomic<bool> sessionDone{ false };    // set by goal complete / escalate / fatal
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
    std::string persistenceDir; ///< Directory for goal state persistence files
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
    return "https://openrouter.ai/api/v1/chat/completions";
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
    CLI::App app("VASDK Call Center Pipeline - goal-oriented voice AI showcase");
    app.allow_extras();
    app.add_option("--ani", args.ani, "Caller ANI (phone number) for CRM lookup + persistence key");
    app.add_option("--persist-dir", args.persistenceDir, "Directory to store partial goal state (enables resume on redial)");
    app.add_option("--llm-endpoint", args.llmEndpoint, "OpenAI-compatible LLM endpoint URL");
    app.add_option("--llm-api-key", args.llmApiKey, "LLM API key (falls back to LLM_API_KEY / OPENROUTER_API_KEY / OPENAI_API_KEY env vars)");
    app.add_option("--llm-model", args.llmModel, "LLM model name (falls back to LLM_MODEL env var)");
    app.add_option("--llm-reasoning-effort", args.llmReasoningEffort,
        "reasoning_effort sent to the LLM, e.g. none for a thinking model (falls back to LLM_REASONING_EFFORT env var)");
    app.add_option("--system-prompt", args.systemPromptFile,
        "Path to a text file containing the call center system prompt (overrides built-in default)");
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

//  Simulated CRM 
// In production replace with a real HTTP call to your CRM / database.

struct CRMRecord {
    std::string accountId;
    std::string name;
    std::string phone;
    std::string accountTier; // "standard" | "premium"
};

std::optional<CRMRecord> crmLookupByPhone(const std::string& phone) {
    // Simulated database - replace with real CRM call
    static const std::vector<CRMRecord> db = {
        {"ACC-001", "Alice Johnson", "555-1234", "premium"},
        {"ACC-002", "Bob Smith",     "555-5678", "standard"},
    };
    for (const auto& r : db) {
        if (r.phone == phone) return r;
    }
    return std::nullopt;
}

std::optional<CRMRecord> crmLookupByAccount(const std::string& accountId) {
    static const std::vector<CRMRecord> db = {
        {"ACC-001", "Alice Johnson", "555-1234", "premium"},
        {"ACC-002", "Bob Smith",     "555-5678", "standard"},
    };
    for (const auto& r : db) {
        if (r.accountId == accountId) return r;
    }
    return std::nullopt;
}

//  Goal definitions 

va::goal::GoalDefinition makeScheduleCallbackGoal() {
    using va::goal::GoalSlot;
    using va::goal::SlotType;

    va::goal::GoalDefinition goal;
    goal.name = "schedule_callback";
    goal.objective = "Schedule a service callback for the customer.";

    GoalSlot customerNameGoalSlot;
    customerNameGoalSlot.name = "customer_name";
    customerNameGoalSlot.description = "the caller's full name";
    customerNameGoalSlot.type = SlotType::STRING;
    customerNameGoalSlot.required = true;

    GoalSlot phoneNumberGoalSlot;
    phoneNumberGoalSlot.name = "phone_number";
    phoneNumberGoalSlot.description = "callback phone number";
    phoneNumberGoalSlot.type = SlotType::PHONE;
    phoneNumberGoalSlot.required = true;

    GoalSlot serviceTypeGoalSlot;
    serviceTypeGoalSlot.name = "service_type";
    serviceTypeGoalSlot.description = "type of service";
    serviceTypeGoalSlot.type = SlotType::ENUM;
    serviceTypeGoalSlot.required = true;
    serviceTypeGoalSlot.constraints.allowedValues = {"repair", "maintenance", "installation"};

    GoalSlot vehicleInfoGoalSlot;
    vehicleInfoGoalSlot.name = "vehicle_info";
    vehicleInfoGoalSlot.description = "vehicle details (object: make, model, year - use update_object_slot)";
    vehicleInfoGoalSlot.type = SlotType::OBJECT;
    vehicleInfoGoalSlot.required = true;
    vehicleInfoGoalSlot.condition = [](const va::goal::GoalState& s) {
        return s.slotValueStr("service_type") == "repair";
    };
    vehicleInfoGoalSlot.constraints.objectSchema = {
        {"make",  "vehicle manufacturer (e.g. Toyota)", "string",  true},
        {"model", "vehicle model (e.g. Camry)",         "string",  true},
        {"year",  "model year (e.g. 2020)",             "integer", true},
    };

    GoalSlot appointmentDateGoalSlot;
    appointmentDateGoalSlot.name = "appointment_date";
    appointmentDateGoalSlot.description = "preferred service date";
    appointmentDateGoalSlot.type = SlotType::DATE;
    appointmentDateGoalSlot.required = true;
    appointmentDateGoalSlot.constraints.mustBeFuture = true;

    goal.slots = {
        customerNameGoalSlot,
        phoneNumberGoalSlot,
        serviceTypeGoalSlot,
        vehicleInfoGoalSlot,
        appointmentDateGoalSlot,
    };

    goal.onComplete = [](const nlohmann::json& slots) -> bool {
        spdlog::info("[CallCenter] Scheduling callback - slots: {}", slots.dump(2));
        // POST to your scheduling API here
        return true;
        };

    return goal;
}

va::goal::GoalDefinition makeAccountInquiryGoal() {
    using va::goal::GoalSlot;
    using va::goal::SlotType;

    va::goal::GoalDefinition goal;
    goal.name = "account_inquiry";
    goal.objective = "Help the caller with an account balance or status inquiry.";

    GoalSlot accountIdSlot;
    accountIdSlot.name = "account_id";
    accountIdSlot.description = "account number or phone number to look up";
    accountIdSlot.type = SlotType::STRING;
    accountIdSlot.required = true;

    // Optional fallback slots - collected when the primary account_id lookup fails.
    // Not required for goal completion; used to attempt a name+DOB lookup instead.
    GoalSlot nameSlot;
    nameSlot.name = "name";
    nameSlot.description = "caller's full name (fallback identifier when account_id lookup fails)";
    nameSlot.type = SlotType::STRING;
    nameSlot.required = false;

    GoalSlot dateOfBirthSlot;
    dateOfBirthSlot.name = "date_of_birth";
    dateOfBirthSlot.description = "caller's date of birth in ISO format YYYY-MM-DD (fallback identifier)";
    dateOfBirthSlot.type = SlotType::DATE;
    dateOfBirthSlot.required = false;

    goal.slots = {
        accountIdSlot,
        nameSlot,
        dateOfBirthSlot,
    };

    goal.onComplete = [](const nlohmann::json& slots) -> bool {
        spdlog::info("[CallCenter] Account inquiry completed - slots: {}", slots.dump(2));
        return true;
        };

    return goal;
}

//  Audio I/O and the call's transport

struct AudioIO {
#ifdef VASDK_HAS_MINIAUDIO
    std::shared_ptr<va::miniaudio::MiniAudioSource> audioSource;
    std::shared_ptr<va::miniaudio::MiniAudioSink>   audioSink;
#elif defined(VASDK_HAS_SDL)
    std::shared_ptr<SDLAudioSource> audioSource;
    std::shared_ptr<SDLAudioSink>   audioSink;
#endif
    std::shared_ptr<va::transport::AudioTransport> transport;    // LocalTransport or WebRtcTransport
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    std::shared_ptr<va::webrtc::LibDataChannelWebRtcProvider> webrtcPeer;
#endif
};

struct GoalLLMBundle {
    std::shared_ptr<va::llm::LLMTransport>          transport;
    std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher;
    std::shared_ptr<va::goal::IntentRouter>         intentRouter;
    std::shared_ptr<va::goal::GoalManager>          goalManager; // set after intent classified
    std::shared_ptr<va::goal::GoalStatePersistence> persistence;
    std::shared_ptr<va::metrics::CallMetrics>       callMetrics;
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

//  Goal activation

struct GoalActivationDeps {
    std::string                                             ani;
    std::shared_ptr<va::metrics::CallMetrics>               metrics;
    std::shared_ptr<va::llm::LLMTransport>                  transport;
    std::shared_ptr<va::llm::ToolDispatchProcessor>         dispatcher;
    std::shared_ptr<va::goal::GoalStatePersistence>         persistence;
    std::shared_ptr<va::goal::IntentRouter>                 router;
    std::shared_ptr<std::shared_ptr<va::goal::GoalManager>> holder;
    std::string                                             systemPromptTemplate;
};

static void activateGoalForIntent(const std::string& intentName,
    const va::goal::GoalDefinition& goalDef,
    const GoalActivationDeps& d)
{
    spdlog::info("[CallCenter] Intent classified: '{}'", intentName);
    d.metrics->intentClassified = intentName;
    d.metrics->goalName = goalDef.name;

    auto gm = std::make_shared<va::goal::GoalManager>(goalDef, /*maxReprompts=*/2);
    *d.holder = gm;
    gm->setMetrics(d.metrics);

    //  Pre-fill from ANI → CRM lookup
    if (!d.ani.empty()) {
        auto record = crmLookupByPhone(d.ani);
        if (record) {
            spdlog::info("[CallCenter] CRM hit for ANI {} → {} ({})",
                d.ani, record->name, record->accountId);
            gm->prefillSlot("customer_name", record->name);
            gm->prefillSlot("phone_number", record->phone);
        }
    }

    //  Restore partial state from previous call
    if (d.persistence && !d.ani.empty()) {
        auto saved = d.persistence->load(d.ani);
        if (saved) {
            spdlog::info("[CallCenter] Resuming partial session for ANI {}", d.ani);
            auto restored = va::goal::deserializeGoalState(goalDef, *saved);
            for (const auto& [name, value] : restored.toJson().items())
                gm->prefillSlot(name, value);
            d.metrics->resumed = true;
        }
    }

    //  Wire into LLM stack (replaces intent-detection prompt)
    {
        auto llmPromptTemplate = va::llm::PromptTemplate::create(d.transport->getPromptChatFormat());
        llmPromptTemplate->setMissingVarPolicy(va::llm::MissingVarPolicy::LEAVE_PLACEHOLDER);
        llmPromptTemplate->addSystem(d.systemPromptTemplate);
        d.transport->setPromptTemplate(std::move(llmPromptTemplate));
    }
    gm->attach(d.transport.get(), d.dispatcher.get());

    d.router->registerSwitchGoalTool(d.dispatcher.get());

    //  Completion callback
    gm->setOnComplete([metrics = d.metrics, persist = d.persistence, ani = d.ani] {
        metrics->goalCompleted = true;
        metrics->callEnd = std::chrono::steady_clock::now();
        metrics->timeToCompletion = metrics->handleTime();

        spdlog::info("[CallCenter] Goal '{}' completed - handle time: {:.1f}s  reprompts: {}",
            metrics->goalName, metrics->handleTime().count(), metrics->repromptCount);

        if (persist && !ani.empty()) persist->clear(ani);
        sessionDone = true;
        });

    //  Escalation callback
    gm->setOnEscalate([metrics = d.metrics, persist = d.persistence, ani = d.ani,
        holder = d.holder]
        (const va::goal::GoalManager::EscalationContext& ctx) {
            spdlog::info("[CallCenter] Escalating - reason: '{}'", ctx.reason);
            spdlog::info("[CallCenter] Collected data for agent: {}",
                ctx.collectedSlots.dump(2));
            metrics->escalated = true;
            metrics->callEnd = std::chrono::steady_clock::now();

            if (persist && !ani.empty()) {
                if (auto gm2 = *holder)
                    persist->save(ani, va::goal::serializeGoalState(gm2->state()));
            }
            sessionDone = true;
        });
}

//  Setup functions

AudioIO createAudioIO(const va::audio::AudioConfig& input, [[maybe_unused]] const CliArgs& args) {
    AudioIO io;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (args.webrtcPort != 0) {
        io.webrtcPeer = std::make_shared<va::webrtc::LibDataChannelWebRtcProvider>();
        io.transport = std::make_shared<va::transport::WebRtcTransport>(io.webrtcPeer);
        return io;
    }
#endif
#ifdef VASDK_HAS_MINIAUDIO
    io.audioSink = std::make_shared<va::miniaudio::MiniAudioSink>();
    io.audioSource = std::make_shared<va::miniaudio::MiniAudioSource>(
        input.sampleRate * input.channels * input.getSampleSizeInBytes() * 30);
#elif defined(VASDK_HAS_SDL)
    (void)input;
    io.audioSink = std::make_shared<SDLAudioSink>();
    io.audioSource = std::make_shared<SDLAudioSource>();
#else
#error "No audio I/O backend - enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL"
#endif
    io.transport = std::make_shared<va::transport::LocalTransport>(io.audioSource, io.audioSink);
    return io;
}

//  Goal-oriented LLM stack 

// The call's own LLM transport: it holds the conversation, and goals swap its prompt.
std::shared_ptr<va::llm::LLMTransport> createLlmTransport(const CliArgs& args) {
    va::llm::LLMConfig config;
    config.endpoint = args.llmEndpoint;
    config.apiKey = args.llmApiKey;
    config.model = args.llmModel;
    config.reasoningEffort = args.llmReasoningEffort;
    if (const char* extra = std::getenv("LLM_EXTRA_PAYLOAD")) config.extraPayload = extra;
    config.temperature = 0.15f;
    config.maxTokens = 512*8;
    config.timeoutMs = 15000;
    config.streaming = true;

    auto transport = std::make_shared<va::llm::OpenAITransport>(config);
    transport->setPromptChatFormat(va::llm::ChatFormat::OPENAI);
    transport->setResponseFormatInstruction(loadOrFallback(args.formatInstructionFile, kCallCenterFormatInstruction));
    transport->onConnected = [] { spdlog::info("[LLM] Connected"); };
    return transport;
}

GoalLLMBundle setupGoals(const CliArgs& args, std::shared_ptr<va::llm::LLMTransport> transport,
                         std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher,
                         const std::string& callId) {
    GoalLLMBundle goalLLMBundle;
    goalLLMBundle.transport = std::move(transport);
    goalLLMBundle.toolDispatcher = std::move(toolDispatcher);
    const std::string systemPromptTmpl = loadOrFallback(args.systemPromptFile, kCallCenterSystemPromptTemplate);

    // CRM lookup tool - available throughout the call (intent + all goals)
    goalLLMBundle.toolDispatcher->registerTool(
        va::llm::Tool("lookup_account",
            "Look up a customer's account details by phone number or account ID. "
            "Use this when the caller provides or confirms their account information.")
        .addParameter({ "identifier", "phone number or account ID", "string", true })
        .onCall([](const nlohmann::json& args) -> nlohmann::json {
            auto id = args["identifier"].get<std::string>();
            auto byPhone = crmLookupByPhone(id);
            auto byAccount = crmLookupByAccount(id);
            auto record = byPhone ? byPhone : byAccount;
            if (!record) return { {"found", false} };
            return {
                {"found",       true},
                {"account_id",  record->accountId},
                {"name",        record->name},
                {"phone",       record->phone},
                {"tier",        record->accountTier},
            };
            })
    );

    // Register {tool_descriptions} resolver once - persists across template swaps
    goalLLMBundle.transport->registerTemplateVar("tool_descriptions",
        [td = goalLLMBundle.toolDispatcher](va::llm::ChatFormat fmt) {
            return td->renderToolDescriptions(fmt);
        });

    //  Call metrics 
    goalLLMBundle.callMetrics = std::make_shared<va::metrics::CallMetrics>();
    goalLLMBundle.callMetrics->callId = callId;
    goalLLMBundle.callMetrics->callerANI = args.ani;
    goalLLMBundle.callMetrics->callStart = std::chrono::steady_clock::now();

    //  Persistence (file-based; Redis in production) 
    if (!args.persistenceDir.empty()) {
        goalLLMBundle.persistence = std::make_shared<va::goal::FileGoalStatePersistence>(
            args.persistenceDir);
        std::filesystem::create_directories(args.persistenceDir);
    }

    //  Intent router with two available goals 
    goalLLMBundle.intentRouter = std::make_shared<va::goal::IntentRouter>(
        std::vector<va::goal::IntentOption>{
            {
                "schedule_callback",
                    "caller wants to schedule a service appointment or callback",
                    makeScheduleCallbackGoal()
            },
            {
                "account_inquiry",
                "caller wants account balance, status, or general account help",
                makeAccountInquiryGoal()
            },
    },
        /* confidenceThreshold = */ 0.65
    );
    goalLLMBundle.intentRouter->setRoleDescription("You are a call center assistant.");
    goalLLMBundle.intentRouter->attachToLLMTransport(goalLLMBundle.transport.get());
    goalLLMBundle.intentRouter->attachToToolDispatcher(goalLLMBundle.toolDispatcher.get());

    //  On intent classified: activate GoalManager
    auto goalManagerHolder = std::make_shared<std::shared_ptr<va::goal::GoalManager>>();

    goalLLMBundle.intentRouter->setOnIntent(
        [deps = GoalActivationDeps{
            args.ani,
            goalLLMBundle.callMetrics,
            goalLLMBundle.transport,
            goalLLMBundle.toolDispatcher,
            goalLLMBundle.persistence,
            goalLLMBundle.intentRouter,
            goalManagerHolder,
            systemPromptTmpl,
        }]
        (const std::string& intentName, const va::goal::GoalDefinition& goalDef) {
            activateGoalForIntent(intentName, goalDef, deps);
        }
    );

    // Expose goalManagerHolder so main can read final state if needed
    goalLLMBundle.goalManager = nullptr; // filled when intent fires; access via goalManagerHolder if needed
    return goalLLMBundle;
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

    spdlog::set_level(args.debug ? spdlog::level::debug : spdlog::level::info);
    spdlog::info("VASDK Call Center Pipeline - goal-oriented voice AI showcase");
    spdlog::info("  ANI:         {}", args.ani.empty() ? "(not set)" : args.ani);
    spdlog::info("  Persist dir: {}", args.persistenceDir.empty() ? "(disabled)" : args.persistenceDir);
    spdlog::info("  LLM model:   {}", args.llmModel.empty() ? "(not set)" : args.llmModel);
    spdlog::info("  LLM endpoint:{}", args.llmEndpoint);
    if (!args.llmReasoningEffort.empty()) spdlog::info("  Reasoning:   {}", args.llmReasoningEffort);

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
        spdlog::error("[CallCenter] Models directory not found");
        return -1;
    }

    // Phone-call turn-taking, streaming STT for speculative firing, and the LLM settings the
    // goal prompts were tuned with. Providers: the first one built in for each kind.
    vasdk::engine::EngineConfig engineConfig;
    engineConfig.mutable_turn_taking()->set_preset(vasdk::engine::TurnTaking::SNAPPY);
    engineConfig.mutable_turn_taking()->set_streaming_transcription(true);
    auto* llmSettings = engineConfig.mutable_llm();
    llmSettings->set_history(12);
    llmSettings->set_speculative_firing(true);   // held until the final transcript matches
    llmSettings->set_speculative_min_words(3);
    // Covers the tool round trip (slot updates, lookups) instead of leaving the caller in silence.
    for (const char* phrase : {"One moment.", "Let me check that.", "Just a second."})
        llmSettings->add_tool_filler_phrases(phrase);

    va::engine::Runtime::Options runtimeOptions;
    runtimeOptions.modelsDir = modelsPath;
    runtimeOptions.frameSize = 16000 / 8;   // 125 ms at 16 kHz
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    std::string error;
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
    callOptions.llmTransport = createLlmTransport(args);
    callOptions.backchannel = args.backchannelConfig;
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    callOptions.sendAllFramesToTransport = io.webrtcPeer != nullptr;   // the page shows the agent's state
#endif
    auto call = runtime->createSession(callOptions, &error);
    if (!call) {
        spdlog::error("[CallCenter] {}", error);
        return -1;
    }
    GoalLLMBundle llm = setupGoals(args, call->llmTransport(), call->toolDispatcher(), callId);

    using Slot = va::engine::Session::Slot;
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // After STT: embedding the whole utterance takes ~70 ms per second of speech.
    auto speakerProvider = std::make_shared<va::thirdparty::PyannoteEmbeddingProvider>(
        modelsPath + "/pyannote_embedding.onnx");
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
        // Wait for the closing TTS phrase to drain. The completion/escalation callback
        // fires mid-LLM-stream, so generation + synthesis + drain can take 10-15s; poll
        // the drain flag (set by TTSFrameProcessor::setOnAudioDrained) with a safety cap
        // instead of a fixed sleep.
        auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!audioFullyDrained &&
            std::chrono::steady_clock::now() < drainDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    spdlog::info("[CallCenter] Stopping pipeline…");
    call->stop();
    spdlog::info("[CallCenter] Done");
    return 0;
}
