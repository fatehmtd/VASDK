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
// Audio, VAD, STT, TTS, and transport setup is copied from VASDK_APP/main_pipeline.cpp.
// 

#include <VASDK/Pipeline.h>
#include <VASDK/AudioRingBuffer.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/TTSFrameProcessor.h>
#include <VASDK/AudioDSPProcessor.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/AgentStateProcessor.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/TransportInputProcessor.h>
#include <VASDK/TransportOutputProcessor.h>
#include <VASDK/RecoveryProcessor.h>
#include <VASDK/FrameProcessor.h>
#include <VASDK/Frame.h>

#include <VASDK/llm/LLMProcessor.h>
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

#include <VASDK/metrics/MetricsCollector.h>
#include <VASDK/metrics/MetricsConfig.h>
#include <VASDK/metrics/MetricsReporter.h>
#include <VASDK/metrics/MetricsBinder.h>
#include <VASDK/metrics/CallMetrics.h>

// Audio I/O
#ifdef VASDK_HAS_MINIAUDIO
#include "MiniAudio/MiniAudioSource.h"
#include "MiniAudio/MiniAudioSink.h"
#elif defined(VASDK_HAS_SDL)
#include "SDL/SDLAudioSource.h"
#include "SDL/SDLAudioSink.h"
#endif

// VAD
#ifdef VASDK_HAS_SILERO_VAD
#include "SileroVAD/SileroVAD.h"
#endif

// STT
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

#ifdef VASDK_HAS_MOONSHINE_STT
#include "MoonshineSTT/MoonshineSttProvider.hpp"
#endif

#ifdef VASDK_HAS_PARAKEET
#include "Parakeet/ParakeetTranscriptionProvider.hpp"
#endif

#ifdef VASDK_HAS_WHISPERCPP
#include "WhisperCpp/WhisperCppTranscriptionProvider.hpp"
#endif

// TTS
#ifdef VASDK_HAS_POCKET_TTS
#include "PocketTTS/PocketVoiceSynthesisProvider.hpp"
#endif

#ifdef VASDK_HAS_SONIOXPP_TTS
#include "SonioxppVoiceSynthesis/SonioxppVoiceSynthesisProvider.h"
#endif

#ifdef VASDK_HAS_SUPERTONIC_TTS
#include "SupertonicTTS/SupertonicVoiceSynthesisProvider.hpp"
#endif

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

// Smart turn detection (optional)
#ifdef VASDK_HAS_SMART_TURN_V3
#include "SmartTurnV3/SmartTurnV3Provider.h"
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
    "Never add keys outside this schema or wrap the JSON in markdown.";

static constexpr const char* kCallCenterSystemPromptTemplate = R"(
You are a helpful call center assistant.

Your goal: {goal_objective}

Information already collected:
{collected_slots}

Current status: {goal_status}

Instructions:
- Collect missing information through natural conversation; do not recite the lists above.
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
{escalation_hint}
Available tools:
{tool_descriptions}
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
    std::string systemPromptFile;
    std::string formatInstructionFile;
    std::string envFile = ".env";
    bool        debug = true;
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

CliArgs parseCliArgs(int argc, char* argv[]) {
    CliArgs args;
    CLI::App app("VASDK Call Center Pipeline - goal-oriented voice AI showcase");
    app.allow_extras();
    app.add_option("--ani", args.ani, "Caller ANI (phone number) for CRM lookup + persistence key");
    app.add_option("--persist-dir", args.persistenceDir, "Directory to store partial goal state (enables resume on redial)");
    app.add_option("--llm-endpoint", args.llmEndpoint, "OpenAI-compatible LLM endpoint URL");
    app.add_option("--llm-api-key", args.llmApiKey, "LLM API key (falls back to LLM_API_KEY / OPENROUTER_API_KEY / OPENAI_API_KEY env vars)");
    app.add_option("--llm-model", args.llmModel, "LLM model name (falls back to LLM_MODEL env var)");
    app.add_option("--system-prompt", args.systemPromptFile,
        "Path to a text file containing the call center system prompt (overrides built-in default)");
    app.add_option("--format-instruction", args.formatInstructionFile,
        "Path to a text file containing the response format instruction (overrides built-in default)");
    app.add_option("--env-file", args.envFile, "Path to .env file (default: .env)");
    app.add_flag("--debug", args.debug, "Enable debug logging");
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

//  Data bundles (mirrors VASDK_APP structure) 

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
    std::shared_ptr<SileroVAD> vadProvider;
#endif
    std::shared_ptr<va::audio::GenericTranscriptionProvider>  transcriptionProvider;
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> ttsProvider;
};

struct GoalLLMBundle {
    va::llm::LLMConfig                              config;
    std::shared_ptr<va::llm::LLMTransport>          transport;
    std::shared_ptr<va::llm::LLMProcessor>          processor;
    std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher;
    std::shared_ptr<va::goal::IntentRouter>         intentRouter;
    std::shared_ptr<va::goal::GoalManager>          goalManager; // set after intent classified
    std::shared_ptr<va::goal::GoalStatePersistence> persistence;
    std::shared_ptr<va::metrics::CallMetrics>       callMetrics;
};

struct TransportBundle {
    std::shared_ptr<va::AudioRingBuffer>           sharedRingBuffer;
    std::shared_ptr<va::AudioRingBuffer>           cleanRingBuffer;
    std::shared_ptr<va::transport::LocalTransport> localTransport;
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

struct ProcessorBundle {
    std::shared_ptr<va::pipeline::TransportInputProcessor>       transportInput;
    std::shared_ptr<va::pipeline::TransportOutputProcessor>      transportOutput;
    std::shared_ptr<va::pipeline::VADFrameProcessor>             vadProcessor;
    std::shared_ptr<va::pipeline::TurnDetectionProcessor>        turnDetection;
    std::shared_ptr<va::pipeline::TranscriptionFrameProcessor>   transcription;
    std::shared_ptr<va::pipeline::AgentStateProcessor>           agentState;
    std::shared_ptr<va::llm::LLMProcessor>                       llmProcessor;
    std::shared_ptr<va::llm::ToolDispatchProcessor>              toolDispatcher;
    std::shared_ptr<va::pipeline::TTSFrameProcessor>             tts;
    std::shared_ptr<SynthesisObserver>                           synthObserver;
    std::shared_ptr<va::RecoveryProcessor>                       recovery;
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

std::optional<AudioBundle> setupAudioProviders(const std::string& modelsPath, int frameSize) {
    AudioBundle b;
    b.frameSize = frameSize;

    b.inputConfig.sampleRate = 16000;
    b.inputConfig.channels = 1;
    b.inputConfig.format = va::audio::AudioFormat::SHORT;
    b.outputConfig.sampleRate = 16000;
    b.outputConfig.channels = 1;
    b.outputConfig.format = va::audio::AudioFormat::SHORT;

#ifdef VASDK_HAS_MINIAUDIO
    b.audioSink = std::make_shared<va::miniaudio::MiniAudioSink>();
    b.audioSource = std::make_shared<va::miniaudio::MiniAudioSource>(
        b.inputConfig.sampleRate * b.inputConfig.channels
        * b.inputConfig.getSampleSizeInBytes() * 30);
#elif defined(VASDK_HAS_SDL)
    b.audioSink = std::make_shared<SDLAudioSink>();
    b.audioSource = std::make_shared<SDLAudioSource>();
#else
#error "No audio I/O backend - enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL"
#endif

#ifdef VASDK_HAS_SILERO_VAD
    b.vadProvider = std::make_shared<SileroVAD>(modelsPath + "/silero_vad.onnx");
    if (!b.vadProvider->initialize(b.inputConfig, b.inputConfig.sampleRate * 4)) {
        spdlog::error("[CallCenter] Failed to initialize Silero VAD");
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
#elif defined(VASDK_HAS_FISHAUDIOPP_STT)
    auto fishaudioStt = std::make_shared<va::thirdparty::FishaudioppTranscriptionProvider>(
        std::getenv("FISHAUDIO_API_KEY") ? std::getenv("FISHAUDIO_API_KEY") : "");
    b.transcriptionProvider = fishaudioStt;
#elif defined(VASDK_HAS_CARTESIAPP_STT)
    auto cartesiaStt = std::make_shared<va::thirdparty::CartesiappTranscriptionProvider>(
        std::getenv("CARTESIA_API_KEY") ? std::getenv("CARTESIA_API_KEY") : "");
    b.transcriptionProvider = cartesiaStt;
#elif defined(VASDK_HAS_MOONSHINE_STT)
    auto moonshine = std::make_shared<MoonshineSttProvider>(modelsPath + "/moonshine");
    MoonshineSttProvider::InferenceConfig sttCfg;
    sttCfg.idleTimeoutMs = 350;
    sttCfg.partialIntervalMs = 80;
    sttCfg.minNewSamplesForPartial = frameSize;
    sttCfg.minNewSamplesForFinal = frameSize;
    moonshine->setInferenceConfig(sttCfg);
    b.transcriptionProvider = moonshine;
#elif defined(VASDK_HAS_PARAKEET)
    auto parakeet = std::make_shared<ParakeetTranscriptionProvider>(modelsPath + "/parakeet");
    ParakeetTranscriptionProvider::InferenceConfig sttCfg;
    sttCfg.idleTimeoutMs = 550;
    sttCfg.partialIntervalMs = 250;
    sttCfg.minNewSamplesForPartial = b.inputConfig.sampleRate / 2;
    sttCfg.minNewSamplesForFinal = sttCfg.minNewSamplesForPartial * 2;
    sttCfg.contextSamples = b.inputConfig.sampleRate / 2;
    sttCfg.silencePaddingSeconds = 1.0f;
    parakeet->setInferenceConfig(sttCfg);
    b.transcriptionProvider = parakeet;
#else
#error "No STT provider - enable VASDK_HAS_SONIOXPP_STT, VASDK_HAS_DEEPGRAMPP_STT, VASDK_HAS_CARTESIAPP_STT, VASDK_HAS_FISHAUDIOPP_STT, VASDK_HAS_GRADIUMPP_STT, VASDK_HAS_MOONSHINE_STT, VASDK_HAS_PARAKEET, or VASDK_HAS_WHISPERCPP"
#endif
    b.transcriptionProvider->setAudioConfiguration(b.inputConfig);

#if defined(VASDK_HAS_GRADIUMPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::GradiumppVoiceSynthesisProvider>(
        std::getenv("GRADIUM_API_KEY") ? std::getenv("GRADIUM_API_KEY") : "",
        std::getenv("GRADIUM_VOICE_ID") ? std::getenv("GRADIUM_VOICE_ID") : "");
#elif defined(VASDK_HAS_SONIOXPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::SonioxppVoiceSynthesisProvider>(
        std::getenv("SONIOX_API_KEY") ? std::getenv("SONIOX_API_KEY") : "");
#elif defined(VASDK_HAS_SUPERTONIC_TTS)
    auto supertonic = std::make_shared<SupertonicVoiceSynthesisProvider>(
        modelsPath + "/supertonic/onnx",
        modelsPath + "/supertonic/voice_styles/M1.json");
    supertonic->setTotalSteps(15);
    b.ttsProvider = supertonic;
#elif defined(VASDK_HAS_DEEPGRAMPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::DeepgramppVoiceSynthesisProvider>(
        std::getenv("DEEPGRAM_API_KEY") ? std::getenv("DEEPGRAM_API_KEY") : "");
#elif defined(VASDK_HAS_FISHAUDIOPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::FishaudioppVoiceSynthesisProvider>(
        std::getenv("FISHAUDIO_API_KEY") ? std::getenv("FISHAUDIO_API_KEY") : "",
        std::getenv("FISHAUDIO_VOICE_ID") ? std::getenv("FISHAUDIO_VOICE_ID") : "");
#elif defined(VASDK_HAS_CARTESIAPP_TTS)
    b.ttsProvider = std::make_shared<va::thirdparty::CartesiappVoiceSynthesisProvider>(
        std::getenv("CARTESIA_API_KEY") ? std::getenv("CARTESIA_API_KEY") : "",
        std::getenv("CARTESIA_VOICE_ID") ? std::getenv("CARTESIA_VOICE_ID") : "");
#elif defined(VASDK_HAS_POCKET_TTS)
    // Local/offline fallback: only used when no cloud TTS provider is compiled in.
    auto pocket = std::make_shared<PocketVoiceSynthesisProvider>(
        modelsPath + "/pocket_tts", "cosette");
    pocket->setVoice("jean");
    pocket->setPreBufferedSamplesMS(250);
    pocket->setSilenceDuration(0.3f);
    b.ttsProvider = pocket;
#else
#error "No TTS provider - enable VASDK_BUILD_SONIOXPP_TTS, VASDK_BUILD_POCKET_TTS, VASDK_BUILD_SUPERTONIC_TTS, VASDK_BUILD_DEEPGRAMPP_TTS, VASDK_BUILD_CARTESIAPP_TTS, VASDK_BUILD_FISHAUDIOPP_TTS, or VASDK_BUILD_GRADIUMPP_TTS"
#endif
    b.ttsProvider->setAudioConfiguration(b.outputConfig);

    return b;
}

//  Goal-oriented LLM stack 

GoalLLMBundle setupGoalLLMStack(const CliArgs& args) {
    GoalLLMBundle goalLLMBundle;

    //  LLM transport (OpenAI-compatible)
    goalLLMBundle.config.endpoint = args.llmEndpoint;
    goalLLMBundle.config.apiKey = args.llmApiKey;
    goalLLMBundle.config.model = args.llmModel;

    goalLLMBundle.config.temperature = 0.15f;
    goalLLMBundle.config.maxTokens = 512;
    goalLLMBundle.config.timeoutMs = 15000;
    goalLLMBundle.config.streaming = true;

    const std::string formatInstruction = loadOrFallback(args.formatInstructionFile, kCallCenterFormatInstruction);
    const std::string systemPromptTmpl  = loadOrFallback(args.systemPromptFile, kCallCenterSystemPromptTemplate);

    goalLLMBundle.transport = std::make_shared<va::llm::OpenAITransport>(goalLLMBundle.config);
    goalLLMBundle.transport->setPromptChatFormat(va::llm::ChatFormat::OPENAI);
    goalLLMBundle.transport->setResponseFormatInstruction(formatInstruction);
    goalLLMBundle.transport->onConnected = [] { spdlog::info("[LLM] Connected"); };
    goalLLMBundle.transport->onError = [](int code, const std::string& msg) {
        spdlog::error("[LLM] Error [{}]: {}", code, msg);
        };

    goalLLMBundle.processor = std::make_shared<va::llm::LLMProcessor>(goalLLMBundle.transport);
    goalLLMBundle.processor->setHistoryEnabled(true);
    goalLLMBundle.processor->setMaxHistoryLength(12);
    goalLLMBundle.processor->setProcessPartialTranscriptions(false);
    goalLLMBundle.processor->setConfidenceThreshold(0.07f);
    goalLLMBundle.processor->setFirePolicy(std::make_shared<va::llm::FinalOnlyFirePolicy>());

    //  Tool dispatcher 
    goalLLMBundle.toolDispatcher = std::make_shared<va::llm::ToolDispatchProcessor>();
    goalLLMBundle.processor->setToolDispatcher(goalLLMBundle.toolDispatcher);

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
    goalLLMBundle.callMetrics->callId = "call-" + std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
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

//  Transport setup (identical to VASDK_APP) 

std::optional<TransportBundle> setupTransport(const AudioBundle& audio) {
    TransportBundle b;
    b.sharedRingBuffer = std::make_shared<va::AudioRingBuffer>(
        (long)audio.inputConfig.sampleRate * 90,
        audio.inputConfig.getSampleSizeInBytes());
    b.cleanRingBuffer = std::make_shared<va::AudioRingBuffer>(
        (long)audio.inputConfig.sampleRate * 90,
        audio.inputConfig.getSampleSizeInBytes());

    b.localTransport = std::make_shared<va::transport::LocalTransport>(
        audio.audioSource, audio.audioSink);
    b.localTransport->setInputAudioConfig(audio.inputConfig);
    b.localTransport->setOutputAudioConfig(audio.outputConfig);
    b.localTransport->setFrameSize(audio.frameSize);
    b.localTransport->setRingBuffer(b.sharedRingBuffer);

    if (!b.localTransport->initialize() || !b.localTransport->start()) {
        spdlog::error("[CallCenter] Failed to start LocalTransport");
        return std::nullopt;
    }
    return b;
}

//  Processor setup 

std::optional<ProcessorBundle> setupProcessors(
    const std::string& modelsPath,
    const AudioBundle& audio,
    const TransportBundle& transport,
    const GoalLLMBundle& llm)
{
    ProcessorBundle b;

    b.transportInput = std::make_shared<va::pipeline::TransportInputProcessor>();
    b.transportInput->addTransport(transport.localTransport);

    b.transportOutput = std::make_shared<va::pipeline::TransportOutputProcessor>();
    b.transportOutput->addTransport(transport.localTransport, va::transport::FrameType::SYNTHESIS);

    // VAD
    b.vadProcessor = std::make_shared<va::pipeline::VADFrameProcessor>(
        audio.vadProvider, 2.0f);
    b.vadProcessor->setMinFrameSize(audio.frameSize);
    va::pipeline::VADConfig vadCfg;
    vadCfg.vadThresholdMs = 150;
    vadCfg.minSpeechDurationMs = 150;
    vadCfg.maxSpeechPauseDurationMs = 200;
    vadCfg.maxSpeechEndDurationMs = 150;
    b.vadProcessor->setConfig(vadCfg);
    b.vadProcessor->setRingBuffer(transport.sharedRingBuffer);

    // Turn detection
#ifdef VASDK_HAS_SMART_TURN_V3
    auto smartTurn = std::make_shared<va::thirdparty::SmartTurnV3Provider>(
        modelsPath + "/smart_turn_v3.onnx", 0.5f, 8.0f, 3);
    if (!smartTurn->initialize(audio.inputConfig, transport.sharedRingBuffer)) {
        spdlog::warn("[CallCenter] SmartTurnV3 init failed - using simple turn detection");
        b.turnDetection = std::make_shared<va::pipeline::TurnDetectionProcessor>();
    }
    else {
        b.turnDetection = std::make_shared<va::pipeline::TurnDetectionProcessor>(smartTurn);
    }
#else
    (void)modelsPath;
    b.turnDetection = std::make_shared<va::pipeline::TurnDetectionProcessor>();
#endif

    // Agent state machine
    b.agentState = std::make_shared<va::pipeline::AgentStateProcessor>();
    b.agentState->setAlwaysOn(true);

    // STT
    b.transcription = std::make_shared<va::pipeline::TranscriptionFrameProcessor>(
        audio.transcriptionProvider);
    b.transcription->configSilenceBehavior(false, false, 1000);
    b.transcription->setStreamingMode(true);
    b.transcription->setRingBuffer(transport.sharedRingBuffer);

    // LLM + tools
    b.llmProcessor = llm.processor;
    b.toolDispatcher = llm.toolDispatcher;

    // TTS
    b.tts = std::make_shared<va::pipeline::TTSFrameProcessor>(audio.ttsProvider);
    b.tts->setPlaybackFlush([lt = transport.localTransport]() { lt->flushOutputAudio(); });

    // Synthesis state observer - intercepts SynthesisStateFrame downstream
    b.synthObserver = std::make_shared<SynthesisObserver>();

    // Recovery processor - sits after STT; catches upstream ErrorFrames
    va::RecoveryProcessor::Config recCfg;
    recCfg.maxSttRetries = 5;
    recCfg.maxLlmRetries = 5;
    recCfg.maxTtsRetries = 2;
    recCfg.sttRetryPrompt = "I'm sorry, I didn't catch that - could you say that again?";
    recCfg.llmRetryPrompt = "Bear with me one moment…";
    recCfg.fatalPrompt = "I'm having some technical difficulties. Let me connect you with an agent.";
    recCfg.onFatalError = [] { sessionDone = true; };
    b.recovery = std::make_shared<va::RecoveryProcessor>(std::move(recCfg));

    return b;
}

//  Pipeline build 

std::shared_ptr<va::pipeline::Pipeline> buildAndStartPipeline(
    const ProcessorBundle& procs)
{
    auto pipeline = std::make_shared<va::pipeline::Pipeline>();

    pipeline->addProcessor(procs.transportInput)
        .addProcessor(procs.vadProcessor)
        .addProcessor(procs.turnDetection)
        .addProcessor(procs.transcription)
        .addProcessor(procs.recovery)       // observes ErrorFrames upstream
        .addProcessor(procs.agentState)
        .addProcessor(procs.llmProcessor)
        .addProcessor(procs.toolDispatcher)
        .addProcessor(procs.tts)
        .addProcessor(procs.synthObserver)
        .addProcessor(procs.transportOutput);

    spdlog::info("[CallCenter] Pipeline built - starting…");

    if (!pipeline->build()) {
        spdlog::error("[CallCenter] Pipeline build failed");
        return nullptr;
    }
    if (!pipeline->start()) {
        spdlog::error("[CallCenter] Pipeline start failed");
        return nullptr;
    }
    spdlog::info("[CallCenter] Pipeline running - speak into microphone");
    return pipeline;
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

    spdlog::set_level(args.debug ? spdlog::level::debug : spdlog::level::info);
    spdlog::info("VASDK Call Center Pipeline - goal-oriented voice AI showcase");
    spdlog::info("  ANI:         {}", args.ani.empty() ? "(not set)" : args.ani);
    spdlog::info("  Persist dir: {}", args.persistenceDir.empty() ? "(disabled)" : args.persistenceDir);
    spdlog::info("  LLM model:   {}", args.llmModel.empty() ? "(not set)" : args.llmModel);
    spdlog::info("  LLM endpoint:{}", args.llmEndpoint);

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

    const int frameSize = 16000 / 8; // 125ms at 16kHz

    auto audioOpt = setupAudioProviders(modelsPath, frameSize);
    if (!audioOpt) return -1;

    GoalLLMBundle llm = setupGoalLLMStack(args);

    auto transportOpt = setupTransport(*audioOpt);
    if (!transportOpt) return -1;

    auto procsOpt = setupProcessors(modelsPath, *audioOpt, *transportOpt, llm);
    if (!procsOpt) return -1;

    // audioFullyDrained is now driven by SynthesisStateFrame flowing downstream
    // through SynthesisObserver (see ProcessorBundle / buildAndStartPipeline).

    auto pipeline = buildAndStartPipeline(*procsOpt);
    if (!pipeline) return -1;

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
    pipeline->stop();
    spdlog::info("[CallCenter] Done");
    return 0;
}
