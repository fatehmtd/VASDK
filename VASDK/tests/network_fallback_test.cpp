// Network fallback: when the network drops mid-conversation, STT and TTS switch to the config's
// local fallbacks between turns and the LLM isn't asked, so the agent keeps answering; when it
// comes back, the cloud providers and the LLM return. A failing cloud provider switches too.
// Each change is a provider_switched event.

#include "engine_test_fakes.h"

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/VAEngine.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/llm/Completion.h>
#include "vasdk/engine/events.pb.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>

using namespace va;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

// An STT that can report a lost connection, and counts its starts.
class CountingStt : public fakes::FakeStt {
public:
    bool startTranscriptionProvider() override {
        ++starts;
        return FakeStt::startTranscriptionProvider();
    }
    void fail() { getTranscriptionResultListener()->onTranscriptionError("connection lost"); }
    std::atomic<int> starts{0};
};

struct ScriptedLlm : llm::Completion {
    mutable std::atomic<int> calls{0};
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>&) const override {
        ++calls;
        return std::string(R"({"intent": "none"})");
    }
};

// A cloud voice whose connection drops while it speaks its first sentence.
class DroppingTts : public fakes::FakeTts {
public:
    ~DroppingTts() override {
        if (_dropper.joinable()) _dropper.join();
    }
    bool synthesizeSpeech(const std::string& text) override {
        if (dropped.exchange(true)) return FakeTts::synthesizeSpeech(text);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            cutOff.push_back(text);
        }
        _dropper = std::thread([this] {
            getVoiceSynthesisResultListener()->onSynthesisStarted();
            std::this_thread::sleep_for(20ms);
            getVoiceSynthesisResultListener()->onSynthesisError("connection lost");
        });
        return true;
    }
    std::vector<std::string> cutOffText() {
        std::lock_guard<std::mutex> lock(_mutex);
        return cutOff;
    }
    std::atomic<bool> dropped{false};

private:
    std::mutex _mutex;
    std::vector<std::string> cutOff;
    std::thread _dropper;
};

// An LLM whose intent checks run past their time limit (slow), or fail at once (unreachable).
struct LateLlm : llm::Completion {
    mutable std::atomic<int> calls{0};
    mutable std::atomic<int> lastLimitMs{-1};
    std::atomic<bool> slow{true};
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override {
        return completeWithin(messages, 0);
    }
    std::optional<std::string> completeWithin(const std::vector<llm::ConversationMessage>&, int timeLimitMs) const override {
        ++calls;
        lastLimitMs = timeLimitMs;
        if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(timeLimitMs + 20));
        return std::nullopt;
    }
};

struct EventRecorder {
    std::mutex mutex;
    std::vector<vasdk::engine::Event> events;
    engine::EventListener listener() {
        return [this](const vasdk::engine::Event& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
    }
    bool waitFor(const std::function<bool(const vasdk::engine::Event&)>& match) {
        for (int i = 0; i < 150; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& event : events) if (match(event)) return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
    int count(const std::function<bool(const vasdk::engine::Event&)>& match) {
        std::lock_guard<std::mutex> lock(mutex);
        int found = 0;
        for (const auto& event : events) found += match(event) ? 1 : 0;
        return found;
    }
};
auto switched(const std::string& kind, const std::string& to) {
    return [kind, to](const vasdk::engine::Event& event) {
        return event.has_provider_switched() && event.provider_switched().kind() == kind && event.provider_switched().to() == to;
    };
}
auto replied(const std::string& text) {
    return [text](const vasdk::engine::Event& event) {
        return event.has_agent_reply() && event.agent_reply().is_final() && event.agent_reply().text() == text;
    };
}
bool spoke(fakes::FakeTts& tts, const std::string& text) {
    for (int i = 0; i < 100; ++i) {
        for (const auto& said : tts.said()) if (said.find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "hours" examples: ["hours", "when are you open"] }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome"
      on_enter { ask { say { text: "How can I help?" } } }
      routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } } } }
)";

} // namespace

int main() {
    auto cloudStt = std::make_shared<CountingStt>();
    auto localStt = std::make_shared<CountingStt>();
    auto cloudTts = std::make_shared<fakes::FakeTts>();
    auto localTts = std::make_shared<fakes::FakeTts>();
    std::atomic<int> localLoads{0};
    engine::ProviderRegistry registry;
    registry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
    registry.add<audio::GenericTranscriptionProvider>("cloud-stt", [&](const engine::ProviderContext&) { return cloudStt; });
    registry.add<audio::GenericTranscriptionProvider>("local-stt", [&](const engine::ProviderContext&) {
        ++localLoads;
        return localStt;
    });
    registry.add<audio::GenericVoiceSynthesisProvider>("cloud-tts", [&](const engine::ProviderContext&) { return cloudTts; });
    registry.add<audio::GenericVoiceSynthesisProvider>("local-tts", [&](const engine::ProviderContext&) { return localTts; });

    vasdk::engine::EngineConfig config;
    std::string error;
    engine::parseProto(R"(
        providers {
          stt { id: "cloud-stt" fallback: ["local-stt"] }
          tts { id: "cloud-tts" fallback: ["local-tts"] }
        })", engine::ProtoFormat::Text, config, &error);
    vasdk::flow::Agent definition;
    engine::parseProto(kAgent, engine::ProtoFormat::Text, definition, &error);
    auto agent = flow::CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;

    EventRecorder events;
    auto llm = std::make_shared<ScriptedLlm>();
    auto transport = std::make_shared<fakes::FakeTransport>();
    VAEngine::Options options;
    options.transport = transport;
    options.agent = agent;
    options.llmCompletion = llm;
    auto session = VAEngine::create(config, registry, options, &error);
    if (!session) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
    session->addListener(events.listener());
    check(session->start(&error), "starts: " + error);
    check(localLoads == 0, "the fallbacks aren't loaded until they're needed");
    check(spoke(*cloudTts, "How can I help?"), "the greeting, by the cloud voice");
    check(events.waitFor([](const auto& event) {
        return event.has_agent_state() && event.agent_state().transition() == "SYNTHESIS_COMPLETED";
    }), "the greeting played to its end");   // a reply playing finishes with its voice
    // Audio reaching the recognizer: a provider is swapped only between turns, as frames pass.
    auto audio = [&] {
        std::vector<char> pcm(3200, 0);
        auto frame = std::make_shared<transport::AudioFrame>();
        frame->numSamples = 1600;
        frame->sampleSizeInBytes = 2;
        frame->postWriteOffset = transport->ringBuffer->write(pcm.data(), 0, 1600);
        transport->onFrameReceived(frame);
    };

    // The network drops: local STT and TTS, no LLM.
    session->setNetworkAvailable(false);
    check(session->usingFallbacks(), "using the fallbacks");
    check(events.waitFor(switched("llm", "off")), "the LLM is off at once");
    check(events.waitFor(switched("stt", "local-stt")) && events.waitFor(switched("tts", "local-tts")),
          "STT and TTS switch to their fallbacks");
    check(localLoads == 1, "the local recognizer is loaded once");
    for (int i = 0; i < 5 && localStt->starts == 0; ++i) {
        audio();
        std::this_thread::sleep_for(50ms);
    }
    check(localStt->starts == 1 && !cloudStt->isTranscriptionProviderRunning(), "the local recognizer listens, the cloud one stopped");
    session->injectUserText("when are you open");
    check(events.waitFor(replied("We're open 8 to 6. How can I help?")) && spoke(*localTts, "We're open 8 to 6."),
          "the agent answers, in the local voice");
    session->injectUserText("my brakes are squealing");
    check(events.waitFor(replied("Sorry, I can't help with that here. How can I help?")), "not understood, offline");
    check(llm->calls == 0, "the LLM isn't asked while the network is down");

    // It comes back: the cloud providers and the LLM again.
    session->setNetworkAvailable(true);
    check(!session->usingFallbacks(), "back on the configured providers");
    check(events.waitFor(switched("llm", "on")) && events.waitFor(switched("stt", "cloud-stt")) &&
              events.waitFor(switched("tts", "cloud-tts")),
          "STT, TTS and the LLM switch back");
    for (int i = 0; i < 5 && !cloudStt->isTranscriptionProviderRunning(); ++i) {
        audio();
        std::this_thread::sleep_for(50ms);
    }
    check(cloudStt->isTranscriptionProviderRunning() && !localStt->isTranscriptionProviderRunning(), "the cloud recognizer listens again");
    auto played = [](const vasdk::engine::Event& event) {
        return event.has_agent_state() && event.agent_state().transition() == "SYNTHESIS_COMPLETED";
    };
    const int playedBefore = events.count(played);
    session->injectUserText("my wipers are streaky");
    for (int i = 0; i < 100 && events.count(replied("Sorry, I can't help with that here. How can I help?")) < 2; ++i)
        std::this_thread::sleep_for(20ms);
    check(llm->calls == 1, "the LLM is asked again: " + std::to_string(llm->calls));
    for (int i = 0; i < 100 && events.count(played) <= playedBefore; ++i) std::this_thread::sleep_for(20ms);
    session->injectUserText("hours");   // the reply before has played: this one is in the cloud voice
    check(spoke(*cloudTts, "We're open 8 to 6."), "in the cloud voice again");

    // A cloud provider that fails switches to its fallback, without turning the LLM off.
    cloudStt->fail();
    check(events.waitFor([](const auto& event) {
        return switched("stt", "local-stt")(event) && event.provider_switched().reason().find("connection lost") != std::string::npos;
    }), "a lost connection switches STT, with the reason");
    check(events.count(switched("llm", "off")) == 1, "the LLM stays on");
    check(localLoads == 1, "the fallbacks loaded before are reused");

    session->stop();

    // The cloud voice drops while it speaks the greeting: the local voice takes over and says it again.
    {
        auto droppingTts = std::make_shared<DroppingTts>();
        auto localVoice = std::make_shared<fakes::FakeTts>();
        engine::ProviderRegistry dropRegistry;
        dropRegistry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
        dropRegistry.add<audio::GenericTranscriptionProvider>("stt", [](const engine::ProviderContext&) {
            return std::make_shared<fakes::FakeStt>();
        });
        dropRegistry.add<audio::GenericVoiceSynthesisProvider>("cloud-tts", [&](const engine::ProviderContext&) { return droppingTts; });
        dropRegistry.add<audio::GenericVoiceSynthesisProvider>("local-tts", [&](const engine::ProviderContext&) { return localVoice; });
        vasdk::engine::EngineConfig dropConfig;
        engine::parseProto(R"(providers { stt { id: "stt" } tts { id: "cloud-tts" fallback: ["local-tts"] } })",
                           engine::ProtoFormat::Text, dropConfig, &error);
        EventRecorder dropEvents;
        VAEngine::Options dropOptions;
        dropOptions.transport = std::make_shared<fakes::FakeTransport>();
        dropOptions.agent = agent;
        auto dropSession = VAEngine::create(dropConfig, dropRegistry, dropOptions, &error);
        if (!dropSession) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        dropSession->addListener(dropEvents.listener());
        check(dropSession->start(&error), "starts: " + error);
        check(dropEvents.waitFor(switched("tts", "local-tts")), "the dropped connection switches TTS");
        check(spoke(*localVoice, "How can I help?"), "the greeting it cut off, in the local voice");
        const auto cut = droppingTts->cutOffText();
        check(cut.size() == 1 && cut[0].find("How can I help?") != std::string::npos, "the cloud voice had started it");
        dropSession->injectUserText("when are you open");
        check(spoke(*localVoice, "We're open 8 to 6."), "and the next reply in the local voice");
        int greetings = 0;
        for (const auto& said : localVoice->said()) greetings += said.find("How can I help?") == 0 ? 1 : 0;
        check(greetings == 1, "said again once");
        dropSession->stop();
    }

    // The intent check has a time limit (llm.intent_timeout_ms): past it, the rules' guess stands
    // and the LLM isn't rested, so the next turn asks it again. A request that fails at once does rest it.
    {
        engine::ProviderRegistry lateRegistry;
        lateRegistry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
        lateRegistry.add<audio::GenericTranscriptionProvider>("stt", [](const engine::ProviderContext&) {
            return std::make_shared<fakes::FakeStt>();
        });
        lateRegistry.add<audio::GenericVoiceSynthesisProvider>("tts", [](const engine::ProviderContext&) {
            return std::make_shared<fakes::FakeTts>();
        });
        vasdk::engine::EngineConfig lateConfig;
        engine::parseProto(R"(
            providers { stt { id: "stt" } tts { id: "tts" } }
            llm { intent_timeout_ms: 50 })", engine::ProtoFormat::Text, lateConfig, &error);
        EventRecorder lateEvents;
        auto late = std::make_shared<LateLlm>();
        VAEngine::Options lateOptions;
        lateOptions.transport = std::make_shared<fakes::FakeTransport>();
        lateOptions.agent = agent;
        lateOptions.llmCompletion = late;
        auto lateSession = VAEngine::create(lateConfig, lateRegistry, lateOptions, &error);
        if (!lateSession) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }
        lateSession->addListener(lateEvents.listener());
        check(lateSession->start(&error), "starts: " + error);
        const auto notUnderstood = [](const vasdk::engine::Event& event) {   // the greeting's question follows when it was asked
            return event.has_agent_reply() && event.agent_reply().is_final() &&
                   event.agent_reply().text().rfind("Sorry, I can't help with that here.", 0) == 0;
        };
        auto ask = [&](const std::string& words, int replies) {
            lateSession->injectUserText(words);
            for (int i = 0; i < 150 && lateEvents.count(notUnderstood) < replies; ++i) std::this_thread::sleep_for(20ms);
            return lateEvents.count(notUnderstood) == replies;
        };
        check(ask("my brakes are squealing", 1) && late->calls == 1 && late->lastLimitMs == 50,
              "asked within the configured limit, then not understood: " + std::to_string(late->lastLimitMs));
        check(ask("my wipers are streaky", 2) && late->calls == 2, "too slow isn't unreachable: asked again");
        late->slow = false;
        check(ask("my horn is quiet", 3) && late->calls == 3, "a request that fails at once");
        check(ask("my seat is wobbly", 4) && late->calls == 3, "rests the LLM: not asked on the next turn");
        lateSession->stop();
    }

    // An LLM-only session (no agent) with the LLM elsewhere: offline, the user hears why there's
    // no answer instead of silence; its offline_replies in turn. One on this machine still answers.
    {
        auto llmOnly = [&](const std::string& endpoint, const std::string& extra, EventRecorder& recorder) {
            engine::ProviderRegistry onlyRegistry;
            onlyRegistry.add<audio::VADProvider>("vad", [](const engine::ProviderContext&) { return std::make_shared<fakes::FakeVad>(); });
            onlyRegistry.add<audio::GenericTranscriptionProvider>("stt", [](const engine::ProviderContext&) {
                return std::make_shared<fakes::FakeStt>();
            });
            onlyRegistry.add<audio::GenericVoiceSynthesisProvider>("tts", [](const engine::ProviderContext&) {
                return std::make_shared<fakes::FakeTts>();
            });
            vasdk::engine::EngineConfig onlyConfig;
            engine::parseProto("providers { stt { id: \"stt\" } tts { id: \"tts\" } } llm { endpoint: \"" + endpoint +
                                   "\" api_key: \"test\" " + extra + " }",
                               engine::ProtoFormat::Text, onlyConfig, &error);
            VAEngine::Options onlyOptions;
            onlyOptions.transport = std::make_shared<fakes::FakeTransport>();
            auto created = VAEngine::create(onlyConfig, onlyRegistry, onlyOptions, &error);
            if (created) created->addListener(recorder.listener());
            return created;
        };
        EventRecorder remoteEvents;
        // 192.0.2.1 (TEST-NET-1): never reached, the network is down before the first turn.
        auto remote = llmOnly("http://192.0.2.1/v1/chat/completions",
                              "offline_replies: [\"No internet, sorry.\", \"Still offline.\"]", remoteEvents);
        check(remote && remote->start(&error), "an LLM-only session starts: " + error);
        if (remote) {
            remote->setNetworkAvailable(false);
            remote->injectUserText("what's the weather like");
            check(remoteEvents.waitFor(replied("No internet, sorry.")), "offline: the user is told, not left waiting");
            remote->injectUserText("and tomorrow");
            check(remoteEvents.waitFor(replied("Still offline.")), "the offline replies in turn");
            remote->stop();
        }
        EventRecorder defaultEvents;
        auto byDefault = llmOnly("https://llm.example.com/v1/chat/completions", "", defaultEvents);
        if (byDefault && byDefault->start(&error)) {
            byDefault->setNetworkAvailable(false);
            byDefault->injectUserText("hello");
            check(defaultEvents.waitFor(replied("I can't reach the internet right now, so I can't answer that. Please try again in a moment.")),
                  "no offline_replies: the default one");
            byDefault->stop();
        }
    }

    if (failures == 0) std::printf("network_fallback_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
