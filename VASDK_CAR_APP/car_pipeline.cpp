//  In-car assistant showcase
//
// A car's voice assistant and its screen:
//   - The agent (agents/car/agent.txtpb): climate, media and phone commands, answered on device
//     with no LLM; with one, what the agent doesn't understand goes to the LLM, which can call
//     the same tools.
//   - The car (core/, verticals/): a simulated one, a vertical per feature (drive, climate,
//     media, phone, messages, navigation, profiles). Voice, the dashboard's buttons and the LLM
//     all change it through the same tools, and it's the agent's $context.car, so its sensors
//     fire triggers: a hot cabin, low fuel, an arrival. A call cuts in on whatever is being said.
//   - The dashboard (dashboard/): the head unit's screen in a browser, on http://127.0.0.1:8090,
//     live: climate, media, the phone, the conversation, and a simulator for the car's sensors.
//
//   VASDK_Car [--config config/engine.txtpb] [--env-file .env] [--dashboard-port 8090] [--webrtc]
// With --webrtc the browser's mic and speaker are the car's, over WebRTC (the dashboard's Talk
// button), instead of this machine's: echo cancellation comes from the browser.

#include <VASDK/VAEngine.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/WebRtcTransport.h>
#include <VASDK/context/ContextProvider.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/engine/Runtime.h>
#include <VASDK/flow/CompiledAgent.h>

#include "DashboardServer.h"
#include "Car.h"
#include "vasdk/engine/events.pb.h"

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

#include <CLI/CLI.hpp>
#include <dotenv.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running{true};

void onSignal(int) { running = false; }

struct CliArgs {
    std::string config = CAR_APP_DIR "/config/engine.txtpb";
    std::string agent = CAR_APP_DIR "/agents/car/agent.txtpb";
    std::string dashboard = CAR_APP_DIR "/dashboard";
    std::string envFile = ".env";
    int dashboardPort = 8090;
    bool debug = false;
    bool webrtc = false;
};

CliArgs parseCliArgs(int argc, char* argv[]) {
    CliArgs args;
    CLI::App app("VASDK Car - an in-car voice assistant with a live dashboard");
    app.add_option("--config", args.config, "Engine config: providers, turn-taking, the LLM")->capture_default_str();
    app.add_option("--agent", args.agent, "The agent file")->capture_default_str();
    app.add_option("--env-file", args.envFile, "Environment file: LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY")->capture_default_str();
    app.add_option("--dashboard-port", args.dashboardPort, "The dashboard, on http://127.0.0.1:<port>")->capture_default_str();
    app.add_option("--dashboard-dir", args.dashboard, "The dashboard's page and scripts (the panels come from verticals/)")->capture_default_str();
    app.add_flag("--debug", args.debug, "Debug logging");
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    app.add_flag("--webrtc", args.webrtc, "Talk from the dashboard over WebRTC instead of the local mic and speaker");
#endif
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::exit(app.exit(e));
    }
    return args;
}

std::string findModelsDirectory(char* argv[]) {
    auto path = std::filesystem::canonical(std::filesystem::path(argv[0])).parent_path();
    for (int i = 0; i <= 3; ++i) {
        if (std::filesystem::exists(path / "VASDK_3P_IMPL/models")) return (path / "VASDK_3P_IMPL/models").string();
        if (!path.has_parent_path() || path == path.parent_path()) break;
        path = path.parent_path();
    }
    return "";
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) : std::string();
}

// The LLM's instructions: prompts/system_prompt.txt, then the prompt.txt of each vertical the agent
// includes, in its order (general knowledge's, say). A vertical dropped from include_files takes
// its part of the instructions with it. The list is read from the agent file itself: once loaded,
// the agent is all its parts merged.
std::string systemPrompt(const std::string& agentFile) {
    std::string prompt = readFile(CAR_APP_DIR "/prompts/system_prompt.txt");
    vasdk::flow::Agent main;
    std::string error;
    if (!va::engine::loadProtoFile(agentFile, main, &error)) return prompt;
    const auto agentDir = std::filesystem::path(agentFile).parent_path();
    for (const auto& part : main.include_files()) {
        const auto file = (agentDir / part).parent_path() / "prompt.txt";
        const auto text = readFile(file.string());
        if (text.empty()) continue;
        prompt += '\n' + text;
        spdlog::info("LLM instructions: + {}", std::filesystem::path(part).parent_path().filename().string());
    }
    return prompt;
}

std::shared_ptr<va::transport::AudioTransport> createLocalAudio(const va::audio::AudioConfig& input) {
#ifdef VASDK_HAS_MINIAUDIO
    auto sink = std::make_shared<va::thirdparty::audio::MiniAudioSink>();
    auto source = std::make_shared<va::thirdparty::audio::MiniAudioSource>();
#elif defined(VASDK_HAS_SDL)
    (void)input;
    auto sink = std::make_shared<va::thirdparty::audio::SDLAudioSink>();
    auto source = std::make_shared<va::thirdparty::audio::SDLAudioSource>();
#else
#error "No audio I/O backend - enable VASDK_BUILD_MINIAUDIO or VASDK_BUILD_SDL"
#endif
    return std::make_shared<va::transport::LocalTransport>(source, sink);
}

// The conversation, for the dashboard and the console.
void forwardEvents(va::VAEngine& engine, car::DashboardServer& dashboard) {
    engine.addListener([&dashboard](const vasdk::engine::Event& event) {
        if (event.has_transcript()) {
            const auto& transcript = event.transcript();
            if (transcript.is_backchannel()) return;
            if (!transcript.is_final()) {
                dashboard.publishHearing(transcript.text());
                return;
            }
            dashboard.publishHearing("");
            if (transcript.text().empty()) return;
            spdlog::info("you: {}", transcript.text());
            dashboard.publish({{"type", "user"}, {"text", transcript.text()}});
        } else if (event.has_agent_reply() && event.agent_reply().is_final()) {
            const auto& reply = event.agent_reply();
            if (reply.text().empty()) return;
            spdlog::info("car: {}", reply.text());
            nlohmann::json message{{"type", "assistant"}, {"text", reply.text()}};
            for (const auto& choice : reply.choices()) message["choices"].push_back(choice);
            for (const auto& value : reply.choice_values()) message["choice_values"].push_back(value);
            if (!reply.ui_json().empty()) message["ui"] = nlohmann::json::parse(reply.ui_json(), nullptr, false);   // a card, for the panels
            dashboard.publish(message);
        } else if (event.has_agent_state()) {
            dashboard.publish({{"type", "agent"}, {"state", event.agent_state().state()}});
        } else if (event.has_agent_turn()) {
            const auto& turn = event.agent_turn();
            dashboard.publish({{"type", "understood"}, {"intent", turn.intent()}, {"matched", turn.matched()},
                               {"page", turn.flow() + "/" + turn.page()}});
        } else if (event.has_trigger_fired()) {
            const auto& trigger = event.trigger_fired();
            spdlog::info("trigger {}: {}", trigger.name(), vasdk::flow::TriggerDecision_Name(trigger.decision()));
            dashboard.publish({{"type", "trigger"}, {"name", trigger.name()},
                               {"decision", vasdk::flow::TriggerDecision_Name(trigger.decision())}});
        } else if (event.has_provider_switched()) {
            const auto& sw = event.provider_switched();
            dashboard.publish({{"type", "provider"}, {"kind", sw.kind()}, {"from", sw.from()}, {"to", sw.to()}, {"reason", sw.reason()}});
        } else if (event.has_error()) {
            dashboard.publish({{"type", "error"}, {"source", event.error().source()}, {"message", event.error().message()}});
        }
    });
}

} // namespace

int main(int argc, char* argv[]) {
    CliArgs args = parseCliArgs(argc, argv);
    spdlog::set_level(args.debug ? spdlog::level::debug : spdlog::level::info);
    if (std::filesystem::exists(args.envFile)) dotenv::init(args.envFile.c_str());   // before the config: it reads ${LLM_*}

    const std::string models = findModelsDirectory(argv);
    if (models.empty()) {
        spdlog::error("Models directory not found (VASDK_3P_IMPL/models, up to 3 levels above the executable)");
        return 1;
    }
    vasdk::engine::EngineConfig config;
    std::string error;
    if (!va::engine::loadEngineConfig(args.config, config, &error)) {
        spdlog::error("{}: {}", args.config, error);
        return 1;
    }

    // The car, and the agent that drives it: every vertical's tools and extractors
    auto vehicle = car::makeCar();
    auto registry = std::make_shared<va::flow::FlowRegistry>();
    vehicle->registerWith(*registry);
    std::vector<std::string> warnings;
    auto agent = va::flow::CompiledAgent::load(args.agent, &error, &warnings, registry);
    for (const auto& warning : warnings) spdlog::warn("{}: {}", args.agent, warning);
    if (!agent) {
        spdlog::error("{}: {}", args.agent, error);
        return 1;
    }

    // $context.car: what the agent's conditions and triggers read, kept up to date
    auto context = std::make_shared<va::context::ContextProvider>();
    context->setValue("car", vehicle->state());

    va::VAEngine::Options options;
    options.id = "car";
    options.modelsDir = models;
    options.agent = agent;
    options.context = context;
    options.systemPrompt = systemPrompt(args.agent);
    options.responseFormatInstruction = readFile(CAR_APP_DIR "/prompts/format_instruction.txt");
    if (!va::engine::applyAudioConfig(config.audio(), options.inputAudio, options.outputAudio, options.frameSize, &error)) {
        spdlog::error("{}: {}", args.config, error);
        return 1;
    }
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    std::shared_ptr<va::thirdparty::transport::LibDataChannelWebRtcProvider> peer;
    if (args.webrtc) {
        peer = std::make_shared<va::thirdparty::transport::LibDataChannelWebRtcProvider>();
        options.transport = std::make_shared<va::transport::WebRtcTransport>(peer);
    } else
#endif
    options.transport = createLocalAudio(options.inputAudio);

    va::engine::ProviderRegistry providers;
    va::thirdparty::registerAllProviders(providers);
    auto engine = va::VAEngine::create(config, providers, options, &error);
    if (!engine) {
        spdlog::error("{}", error);
        return 1;
    }
    for (const auto& tool : vehicle->tools()) engine->registerTool(tool);   // the LLM drives the same car
    // What the verticals may ask of the conversation: a call rang, a message came, a driver got in
    car::Conversation conversation;
    conversation.fireTrigger = [&engine](const std::string& name, const google::protobuf::Struct& payload) {
        engine->fireTrigger(name, payload);
    };
    conversation.setSessionParam = [&engine](const std::string& name, const google::protobuf::Value& value) {
        engine->setSessionParam(name, value);
    };
    vehicle->setConversation(conversation);

    // The dashboard
    car::DashboardServer::Handlers handlers;
    handlers.state = [vehicle] { return vehicle->state(); };
    handlers.info = [vehicle, webrtc = args.webrtc] { return nlohmann::json{{"verticals", vehicle->names()}, {"webrtc", webrtc}}; };
    handlers.control = [vehicle](const nlohmann::json& command) { return vehicle->control(command); };
    handlers.say = [&engine](const std::string& text) { engine->injectUserText(text); };   // shown as its transcript
    handlers.simulate = [vehicle](const nlohmann::json& command) { return vehicle->simulate(command); };
    handlers.stop = [&engine] { engine->interrupt(); };
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    if (peer) {
        handlers.offer = [peer](const std::string& offer, int& status) {
            if (peer->isConnected()) {
                status = 409;
                return std::string();
            }
            auto answer = peer->handleOffer(offer);
            if (answer.empty()) status = 400;
            return answer;
        };
    }
#endif
    // The page, its script, and each vertical's panel beside its code
    std::map<std::string, std::string> files{{"/", args.dashboard + "/index.html"}, {"/app.js", args.dashboard + "/app.js"},
                                             {"/style.css", args.dashboard + "/style.css"}};
    for (const auto& name : vehicle->names())
        files["/verticals/" + name + "/panel.js"] = std::string(CAR_APP_DIR "/verticals/") + name + "/panel.js";
    car::DashboardServer dashboard(files, handlers);
    vehicle->onChange([context, &dashboard](const nlohmann::json& state) {
        context->setValue("car", state);
        dashboard.publishState(state);
    });
    dashboard.publishState(vehicle->state());
    forwardEvents(*engine, dashboard);

    if (!engine->start(&error)) {
        spdlog::error("{}", error);
        return 1;
    }
    if (!dashboard.start("127.0.0.1", args.dashboardPort, &error)) {
        spdlog::error("Dashboard: {}", error);
        engine->stop();
        return 1;
    }
    spdlog::info("VASDK Car - {}", engine->describe());
    spdlog::info("  LLM:       {}", engine->hasLLM() ? config.llm().endpoint() + " (" + config.llm().model() + ")" : "none: the agent alone");
    spdlog::info("  Dashboard: http://127.0.0.1:{}{}", args.dashboardPort, args.webrtc ? " (press Talk: WebRTC)" : "");
    spdlog::info("Say \"I'm cold\", \"play some jazz\" or \"what can you do\". Ctrl+C to stop.");

    std::signal(SIGINT, onSignal);
    auto last = std::chrono::steady_clock::now();
    while (running) {   // the car's time: the cabin warms or cools, the track plays on
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        vehicle->tick(std::chrono::duration<double>(now - last).count());
        last = now;
    }
    spdlog::info("Stopping");
    dashboard.stop();
    engine->stop();
    return 0;
}
