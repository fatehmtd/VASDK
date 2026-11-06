// A part of a reply in another voice or at another pace: say { voice speed } gives the reply's
// segments their style (alike parts still join), a speed out of range fails at load, and
// QueuedVoiceSynthesisProvider hands each text's style to the engine, back to its own after.

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

const char* kAgent = R"agent(
start_flow: "main"
flows { name: "main" start_page: "welcome" pages {
  name: "welcome"
  on_enter { say { text: "Hello." } }
  on_enter { say { text: "Welcome to Acme." } }
  on_enter { say { text: "Calls are recorded." voice: "cosette" speed: 1.3 } }
  on_enter { ask { say { text: "How can I help?" } } }
} }
)agent";

std::shared_ptr<const flow::CompiledAgent> compileText(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return flow::CompiledAgent::compile(agent, error);
}

// Says nothing; records each text with the style it was to be said in.
class RecordingVoice : public audio::QueuedVoiceSynthesisProvider {
public:
    RecordingVoice() : QueuedVoiceSynthesisProvider("RecordingVoice", Delivery::Direct) {}
    ~RecordingVoice() override { stopVoiceSynthesisProvider(); }

    std::vector<std::pair<std::string, audio::SpeechStyle>> said() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _said;
    }
    bool waitFor(size_t count) {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5), [&] { return _said.size() >= count; });
    }

private:
    bool loadEngine() override { return true; }
    bool synthesizeOne(const std::string& text) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _said.emplace_back(text, style());
        _changed.notify_all();
        return true;
    }
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<std::pair<std::string, audio::SpeechStyle>> _said;
};

} // namespace

int main() {
    std::string error;
    auto agent = compileText(kAgent, &error);
    check(agent != nullptr, "the agent compiles");
    if (agent) {
        flow::FlowRuntime runtime(agent);
        const auto reply = runtime.start();
        const auto& segments = reply.segments;
        check(segments.size() == 3, "alike parts join; a styled one is its own");
        if (segments.size() == 3) {
            check(segments[0].text == "Hello. Welcome to Acme." && segments[0].voice.empty() && segments[0].speed == 0,
                  "the voice's own way");
            check(segments[1].text == "Calls are recorded." && segments[1].voice == "cosette" && segments[1].speed > 1.29f &&
                      segments[1].speed < 1.31f,
                  "another voice, faster");
            check(segments[2].text == "How can I help?" && segments[2].voice.empty(), "then its own again");
        }
    }
    std::string text = kAgent;
    text.replace(text.find("speed: 1.3"), 10, "speed: 3");
    check(!compileText(text, &error) && error.find("speed must be 0.5 to 2") != std::string::npos, "a speed out of range fails");

    RecordingVoice voice;
    audio::AudioConfig config;
    config.sampleRate = 16000;
    config.channels = 1;
    voice.setAudioConfiguration(config);
    check(voice.startVoiceSynthesisProvider(), "the voice starts");
    voice.synthesizeSpeech("Hello.");
    voice.synthesizeStyledSpeech("Calls are recorded.", {"cosette", 1.3f});
    voice.synthesizeSpeech("How can I help?");
    check(voice.waitFor(3), "all three said");
    const auto said = voice.said();
    check(said.size() == 3 && said[0].second.isDefault() && said[1].second.voice == "cosette" && said[1].second.speed > 1.29f &&
              said[2].second.isDefault(),
          "each text with its style, and the voice's own after");

    if (failures == 0) std::printf("speech_style_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
