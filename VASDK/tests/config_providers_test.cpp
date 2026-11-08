// What an engine config can now name and a Command can now ask, so every binding gets it through
// the config or vasdk_session_send: the device's audio (providers.audio_input / audio_output), an
// LLM from a provider (providers.llm, with the agent's one-shot asks when it answers them), the
// session controls that were C++-only (reset, push-to-talk, follow-up, entity values,
// back-channel), and a question asked through a StepOutcome (what a C, Python or Kotlin step does).

#include "engine_test_fakes.h"

#include <VASDK/GenericAudioSink.h>
#include <VASDK/GenericAudioSource.h>
#include <VASDK/VAEngine.h>
#include <VASDK/engine/Runtime.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRegistry.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/llm/Completion.h>
#include <VASDK/llm/LLMTransport.h>
#include "vasdk/engine/events.pb.h"
#include "vasdk/flow/step_outcome.pb.h"

#include <google/protobuf/text_format.h>

#include <cstdio>

using namespace va;
using namespace fakes;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct Devices {
    std::atomic<bool> sourceOpened{false}, sinkOpened{false};
};

class FakeSource : public audio::GenericAudioSource {
public:
    explicit FakeSource(std::shared_ptr<Devices> devices) : _devices(std::move(devices)) {}
    bool isAudioSourceReady() const override { return true; }
    bool initializeAudioSource(const audio::AudioConfig&) override { return _devices->sourceOpened = true; }
    void releaseAudioSource() override {}
    int readAudio(char*, int, int) override { return 0; }
    bool startAudioSource() override { return true; }
    void stopAudioSource() override {}
    bool isAudioSourceStarted() const override { return true; }
private:
    std::shared_ptr<Devices> _devices;
};

class FakeSink : public audio::GenericAudioSink {
public:
    explicit FakeSink(std::shared_ptr<Devices> devices) : _devices(std::move(devices)) {}
    bool isAudioSinkReady() const override { return true; }
    bool initializeAudioSink(const audio::AudioConfig&) override { return _devices->sinkOpened = true; }
    void releaseAudioSink() override {}
    int writeAudio(const char*, int, int length) override { return length; }
    bool startAudioSink() override { return true; }
    void stopAudioSink() override {}
    bool isAudioSinkStarted() const override { return true; }
private:
    std::shared_ptr<Devices> _devices;
};

class FakeLlm : public llm::LLMTransport {
public:
    FakeLlm() : LLMTransport(llm::LLMProvider::CUSTOM, llm::LLMConfig{}) {}
protected:
    std::string sendLLMRequest(const std::string&) override { return "Sure."; }
};

class FakeCompletion : public llm::Completion {
public:
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>&) const override { return "yes"; }
};

engine::ProviderRegistry registryWith(std::shared_ptr<FakeTts> tts, std::shared_ptr<Devices> devices, bool completion) {
    auto registry = makeRegistry(std::move(tts));
    registry.add<audio::GenericAudioSource>("device", [devices](const engine::ProviderContext&) {
        return std::make_shared<FakeSource>(devices); });
    registry.add<audio::GenericAudioSink>("device", [devices](const engine::ProviderContext&) {
        return std::make_shared<FakeSink>(devices); });
    registry.add<llm::LLMTransport>("local", [](const engine::ProviderContext&) { return std::make_shared<FakeLlm>(); });
    if (completion)
        registry.add<llm::Completion>("local", [](const engine::ProviderContext&) { return std::make_shared<FakeCompletion>(); });
    return registry;
}

void testAudioDevices() {
    auto devices = std::make_shared<Devices>();
    const auto registry = registryWith(std::make_shared<FakeTts>(), devices, false);
    vasdk::engine::EngineConfig config;
    config.mutable_providers()->mutable_audio_input()->set_id("device");
    config.mutable_providers()->mutable_audio_output()->set_id("device");
    std::string error;
    auto engine = VAEngine::create(config, registry, {}, &error);   // no transport: the config's devices
    check(engine != nullptr, "the config's audio devices make the transport");
    if (!engine) { std::fprintf(stderr, "  %s\n", error.c_str()); return; }
    check(engine->start(&error), "a session on the config's devices starts");
    check(devices->sourceOpened && devices->sinkOpened, "the session opened the microphone and the speaker");
    engine->stop();

    vasdk::engine::EngineConfig half;
    half.mutable_providers()->mutable_audio_input()->set_id("device");
    check(!VAEngine::create(half, registry, {}, &error) && contains(error, "go together"),
          "audio_input without audio_output is refused");
    vasdk::engine::EngineConfig unknown;
    unknown.mutable_providers()->mutable_audio_input()->set_id("nothing");
    unknown.mutable_providers()->mutable_audio_output()->set_id("device");
    check(!VAEngine::create(unknown, registry, {}, &error) && contains(error, "audio_input"),
          "an audio_input that isn't built in says which");
    check(!VAEngine::create(vasdk::engine::EngineConfig{}, registry, {}, &error) && contains(error, "transport is required"),
          "no devices and no transport: still an error");
}

void testLlmProvider() {
    vasdk::engine::EngineConfig config;
    config.mutable_providers()->mutable_llm()->set_id("local");
    std::string error;

    auto runtime = engine::Runtime::create(config, registryWith(std::make_shared<FakeTts>(), std::make_shared<Devices>(), true), {}, &error);
    check(runtime != nullptr, "a runtime with providers.llm");
    if (!runtime) return;
    auto llm = runtime->createLlm(&error);
    check(llm.transport && llm.completion, "providers.llm gives the conversation's LLM and the agent's");

    auto noAsks = engine::Runtime::create(config, registryWith(std::make_shared<FakeTts>(), std::make_shared<Devices>(), false), {}, &error);
    llm = noAsks->createLlm(&error);
    check(llm.transport && !llm.completion && error.empty(), "a provider without one-shot asks: the conversation's only");

    VAEngine::Options options;
    options.transport = std::make_shared<FakeTransport>();
    auto engine = VAEngine::create(config, registryWith(std::make_shared<FakeTts>(), std::make_shared<Devices>(), false), options, &error);
    check(engine && engine->hasLLM(), "a session's LLM from providers.llm");

    vasdk::engine::EngineConfig missing;
    missing.mutable_providers()->mutable_llm()->set_id("nowhere");
    options.transport = std::make_shared<FakeTransport>();
    check(!VAEngine::create(missing, registryWith(std::make_shared<FakeTts>(), std::make_shared<Devices>(), false), options, &error) &&
              contains(error, "LLM"),
          "a providers.llm that isn't built in fails the session");
}

void testCommands() {
    VAEngine::Options options;
    options.transport = std::make_shared<FakeTransport>();
    std::string error;
    auto engine = VAEngine::create(vasdk::engine::EngineConfig{}, makeRegistry(std::make_shared<FakeTts>()), options, &error);
    check(engine != nullptr, "an engine for the commands");
    if (!engine) return;

    vasdk::engine::Command command;
    command.mutable_set_backchannel()->set_enabled(false);
    command.mutable_set_backchannel()->set_duck_ms(1234);
    check(engine->handle(command, &error), "set_backchannel is handled");
    check(!engine->backchannel().enabled && engine->backchannel().duckDurationMs == 1234, "set_backchannel's fields apply");
    command.Clear();
    command.mutable_set_backchannel()->set_duck_gain_db(-20.0f);
    engine->handle(command, &error);
    check(!engine->backchannel().enabled && engine->backchannel().duckDurationMs == 1234 &&
              engine->backchannel().duckGainDecibels == -20.0f,
          "a later set_backchannel keeps the fields it doesn't set");

    command.Clear();
    command.set_push_to_talk(true);
    check(engine->handle(command, &error), "push_to_talk is handled");
    command.Clear();
    command.set_follow_up_listen_ms(4000);
    check(engine->handle(command, &error), "follow_up_listen_ms is handled");
    command.Clear();
    command.mutable_set_entity_values()->set_name("contact");
    command.mutable_set_entity_values()->add_values()->set_value("mom");
    check(engine->handle(command, &error), "set_entity_values is handled (without an agent, logged)");
    command.Clear();
    command.set_reset_conversation(true);
    check(engine->handle(command, &error), "reset_conversation is handled");
    check(!engine->handle(vasdk::engine::Command{}, &error), "an empty command is still refused");
}

void testStepOutcomeAsk() {
    vasdk::flow::Agent definition;
    google::protobuf::TextFormat::ParseFromString(R"(
        start_flow: "main"
        flows { name: "main" start_page: "pick" pages { name: "pick" type: "asker" } }
    )", &definition);
    auto registry = std::make_shared<flow::FlowRegistry>();
    flow::FlowRegistry::PageType asker;
    asker.onEnter = [](const google::protobuf::Message&, flow::TurnContext& turn) {
        vasdk::flow::StepOutcome outcome;
        outcome.add_say("Hello.");
        outcome.mutable_ask()->set_question("Tea or coffee?");
        outcome.mutable_ask()->add_choices()->set_label("Tea");
        outcome.mutable_ask()->add_choices()->set_label("Coffee");
        std::string problem;
        turn.apply(outcome, &problem);
    };
    registry->addPageType("asker", asker);
    std::string error;
    auto agent = flow::CompiledAgent::compile(definition, &error, nullptr, registry);
    check(agent != nullptr, "an agent whose page asks through a StepOutcome");
    if (!agent) { std::fprintf(stderr, "  %s\n", error.c_str()); return; }
    flow::FlowRuntime conversation(agent);
    const auto reply = conversation.start();
    check(contains(reply.text, "Hello.") && contains(reply.text, "Tea or coffee?"), "the says, then the question");
    check(reply.expectsReply && reply.choices.size() == 2, "the question expects an answer and offers its choices");
}

} // namespace

int main() {
    testAudioDevices();
    testLlmProvider();
    testCommands();
    testStepOutcomeAsk();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("config providers ok");
    return 0;
}
