// Uses the installed C library from C++ through the header-only wrapper, VASDK/c/vasdk.hpp: a tool
// written in C++, an agent and a conversation, the providers built in and the model store.
#include <VASDK/c/vasdk.hpp>
#include <VASDK/version.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

const char* kAgent =
    "start_flow: \"main\"\n"
    "session_params { name: \"booking\" type: STRUCT }\n"
    "intents { name: \"book\" examples: [\"book\", \"book a service\"] }\n"
    "flows { name: \"main\" start_page: \"welcome\" pages {\n"
    "  name: \"welcome\"\n"
    "  on_enter { ask { say { text: \"Hi, how can I help?\" } } }\n"
    "  routes { intent: \"book\" steps { call_tool { name: \"create_booking\" args { key: \"service\" value: \"'oil'\" }\n"
    "    into: \"$session.booking\" } } steps { say { text: \"Booked $session.booking.reference.\" } } } } }\n";

} // namespace

int main() {
    check(vasdk::version() == VASDK_VERSION_STRING, "version " + vasdk::version());
    check(vasdk::apiVersion() == VASDK_C_API_VERSION, "C API version");

    // A tool in C++, the agent that calls it, a conversation
    int bookings = 0;
    vasdk::Registry registry;
    registry.addTool("create_booking", "Books a service", [&](std::string_view arguments) {
        ++bookings;
        return std::string(arguments.find("oil") != std::string_view::npos ? R"({"reference": "B-1"})" : R"({})");
    });
    try {
        const vasdk::Agent agent = vasdk::Agent::parse(kAgent, VASDK_FORMAT_TEXT, &registry);
        vasdk::Conversation conversation(agent);
        check(conversation.start().find("Hi, how can I help?") != std::string::npos, "greeting");
        const std::string reply = conversation.say("book a service");
        check(reply.find("Booked B-1.") != std::string::npos && bookings == 1, "the tool ran: " + reply);
    } catch (const vasdk::Error& error) {
        check(false, std::string("agent: ") + error.what());
    }
    bool threw = false;
    try {
        vasdk::Agent::parse("flows {", VASDK_FORMAT_TEXT);
    } catch (const vasdk::Error&) {
        threw = true;
    }
    check(threw, "a broken agent throws vasdk::Error");

    const vasdk::Providers providers = vasdk::Providers::builtIn();
    check(providers.get() != nullptr, "providers");

    // The model store: its manifest, a config's models, statuses (nothing downloaded here)
    const auto directory = std::filesystem::temp_directory_path() / "vasdk_consumer_models";
    std::filesystem::create_directories(directory);
    const vasdk::Models models(R"({"directory": ")" + directory.generic_u8string() + R"("})");
    check(models.directory() == directory.generic_u8string(), "models directory");
    check(models.list().find("\"silero_vad\"") != std::string::npos, "the built-in manifest lists silero_vad");
    check(models.status("no_such_model") == VASDK_MODEL_UNKNOWN, "unknown model");
    std::ofstream(directory / "engine.txtpb") << "providers { vad { id: \"silero\" } turn { id: \"smartturn-v3\" } }\n";
    const auto needed = models.forConfig((directory / "engine.txtpb").u8string());
    check(needed.size() == 2 && needed[0] == "silero_vad" && needed[1] == "smart_turn_v3", "models for a config");
    threw = false;
    try {
        models.pull("no_such_model");
    } catch (const vasdk::Error&) {
        threw = true;
    }
    check(threw, "pulling an unknown model throws");
    std::filesystem::remove_all(directory);

    if (failures == 0) std::printf("VASDK %s from C++ (vasdk.hpp): ok\n", vasdk::version().c_str());
    return failures == 0 ? 0 : 1;
}
