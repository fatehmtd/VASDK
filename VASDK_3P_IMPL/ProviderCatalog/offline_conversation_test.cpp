// A spoken conversation with no network: Silero VAD, Whistle STT (Parakeet where Whistle isn't
// built, as on the x86_64 Android emulator) and Pocket TTS, on this machine.
// The user's lines are spoken by a second voice (Phonon) and pushed in as audio, a 100 ms frame
// every 100 ms (a microphone's pace), through a C API session (vasdk_session), so the whole path
// runs: VAD, STT, the agent's turn, its spoken reply. The assistant's speech isn't fed back
// (headphones). Exits 77 (skipped) when the models aren't there.
//   offline_conversation_test <models dir> [assistant voice: pocket] [stt: whistle, else parakeet]

#include "ProviderCatalog.h"

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>
#include "vasdk/engine/engine_config.pb.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::fprintf(ok ? stdout : stderr, "%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// The user's voice: a TTS whose audio is gathered until it's done.
class UserVoice : public va::audio::VoiceSynthesisResultListener {
public:
    explicit UserVoice(std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> tts) : _tts(std::move(tts)) {
        _tts->setVoiceSynthesisResultListener(this);
    }
    std::vector<short> say(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pcm.clear();
            _done = false;
        }
        _tts->synthesizeSpeech(text);
        std::unique_lock<std::mutex> lock(_mutex);
        _changed.wait_for(lock, 60s, [this] { return _done; });
        return _pcm;
    }
    void onSynthesisStarted() override {}
    void onSynthesisCompleted() override {
        std::lock_guard<std::mutex> lock(_mutex);
        _done = true;
        _changed.notify_all();
    }
    void onSynthesisError(const std::string& error) override {
        std::fprintf(stderr, "user voice: %s\n", error.c_str());
        onSynthesisCompleted();
    }
    void onAudioDataReceived(const char* data, size_t size) override {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto* samples = reinterpret_cast<const short*>(data);
        _pcm.insert(_pcm.end(), samples, samples + size / sizeof(short));
    }

private:
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> _tts;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<short> _pcm;
    bool _done = false;
};

// What the session said and did, from its events.
struct Conversation {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> replies;   // the agent's final replies
    std::vector<std::string> heard;     // final transcripts
    std::vector<std::string> pages;
    int replyEnds = 0;                  // SYNTHESIS_COMPLETED: a reply played to its end

    void onEvent(const nlohmann::json& event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.contains("agentReply") && event["agentReply"].value("isFinal", false))
            replies.push_back(event["agentReply"].value("text", std::string()));
        if (event.contains("transcript") && event["transcript"].value("isFinal", false))
            heard.push_back(event["transcript"].value("text", std::string()));
        if (event.contains("pageChanged")) pages.push_back(event["pageChanged"].value("toPage", std::string()));
        if (event.contains("agentState") && event["agentState"].value("transition", std::string()) == "SYNTHESIS_COMPLETED") ++replyEnds;
        changed.notify_all();
    }
    // Waits for reply number `count` to have played; its text, or "" on a timeout.
    std::string replyPlayed(size_t count, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, timeout, [&] { return replies.size() >= count && replyEnds >= static_cast<int>(count); });
        return replies.size() >= count ? replies[count - 1] : std::string();
    }
    std::string lastHeard() {
        std::lock_guard<std::mutex> lock(mutex);
        return heard.empty() ? std::string() : heard.back();
    }
};

void onEvent(const char* json, void* user) {
    static_cast<Conversation*>(user)->onEvent(nlohmann::json::parse(json, nullptr, false));
}

// A microphone: a 100 ms frame every 100 ms, the user's words when they speak, silence otherwise.
class Microphone {
public:
    Microphone(vasdk_session* session, int rate) : _session(session), _frame(static_cast<size_t>(rate / 10)) {
        _thread = std::thread([this] { run(); });
    }
    ~Microphone() {
        _stop = true;
        _thread.join();
    }
    void speak(const std::vector<short>& pcm) {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.insert(_queue.end(), pcm.begin(), pcm.end());
    }
    bool speaking() {
        std::lock_guard<std::mutex> lock(_mutex);
        return !_queue.empty();
    }

private:
    void run() {
        std::vector<short> frame(_frame);
        auto next = std::chrono::steady_clock::now();
        while (!_stop) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& sample : frame) {
                    sample = _queue.empty() ? 0 : _queue.front();
                    if (!_queue.empty()) _queue.pop_front();
                }
            }
            vasdk_session_push_audio(_session, frame.data(), frame.size());
            next += 100ms;
            std::this_thread::sleep_until(next);
        }
    }
    vasdk_session* _session;
    size_t _frame;
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    std::deque<short> _queue;
};

bool mentions(const std::string& text, std::initializer_list<const char*> words) {
    std::string lower;
    for (const char c : text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* word : words)
        if (lower.find(word) != std::string::npos) return true;
    return false;
}

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "hours" examples: ["when are you open", "opening hours", "what time do you open"] }
  intents { name: "book" examples: ["book", "book an @service", "I'd like to book an @service", "appointment"] }
  intents { name: "yes" examples: ["yes", "yes please", "that's right"] }
  entity_types { name: "service" list {
    values { value: "oil_change" spoken: "an oil change" synonyms: ["oil change", "oil"] }
    values { value: "tire_repair" spoken: "a tire repair" synonyms: ["tire repair", "tires", "tyres"] } } }
  goals { name: "booking" require_confirmation: true
    confirm: "So that's $form.service on $form.day. Shall I book it?"
    slots { name: "service" entity: "@service" prompt: "Which service do you need?" }
    slots { name: "day" entity: "@sys.date" prompt: "Which day would suit you?" } }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome"
      on_enter { ask { say { text: "Hello, this is the garage. How can I help?" } } }
      routes { intent: "hours" steps { say { text: "We're open from 8 to 6, Monday to Saturday." } } }
      routes { intent: "book" target: "booking" } }
    pages { name: "booking" form { goal: "booking" } routes { condition: "$form.confirmed" target: "booked" } }
    pages { name: "booked" on_enter { say { text: "You're booked. See you then." } } on_enter { end {} } } }
)";

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: offline_conversation_test <models dir>\n");
        return 2;
    }
    const std::filesystem::path models = std::filesystem::absolute(argv[1]);
    vasdk_providers* providers = vasdk_providers_create();
    vasdk_providers_add_all(providers);
    // The recognizer: as asked, else Whistle when this binary has it, else Parakeet.
    const auto recognizers = vasdk_providers_registry(providers).ids<va::audio::GenericTranscriptionProvider>();
    auto built = [&](const std::string& id) { return std::find(recognizers.begin(), recognizers.end(), id) != recognizers.end(); };
    const std::string stt = argc > 3 ? argv[3] : built("whistle") ? "whistle" : "parakeet";
    if (!built(stt)) {
        std::printf("skipped: no %s recognizer in this binary\n", stt.c_str());
        return 77;
    }
    const std::string sttModel = stt == "whistle" ? "whistle/whistle.cact" : stt + "/vocab.txt";
    for (const std::string& needed : {std::string("silero_vad.onnx"), sttModel, std::string("pocket_tts/voices.bin"),
                                      std::string("phonon_tts")}) {
        if (!std::filesystem::exists(models / needed)) {
            std::printf("skipped: %s isn't in %s\n", needed.c_str(), models.string().c_str());
            return 77;
        }
    }
    const auto dir = std::filesystem::temp_directory_path() / "vasdk_offline_conversation";
    std::filesystem::create_directories(dir);
    const auto configPath = (dir / "engine.txtpb").string();
    const std::string assistantVoice = argc > 2 ? argv[2] : "pocket";   // offline_conversation_test <models> phonon
    // The voice through its own options message, the STT through the shared one older configs use: both paths.
    const std::string voiceOptions = assistantVoice == "phonon" ? "PhononOptions" : "PocketOptions";
    std::ofstream(configPath) << "models_dir: \"" << models.generic_string() << "\"\n"
                              << "providers { vad { id: \"silero\" }\n"
                              << "  stt { id: \"" << stt << "\" options { [type.googleapis.com/vasdk.providers.LocalSttOptions] { idle_timeout_ms: 550 } } }\n"
                              << "  tts { id: \"" << assistantVoice << "\" options { [type.googleapis.com/vasdk.providers." << voiceOptions
                              << "] { prebuffer_ms: 250 silence_duration_seconds: 0.3 } } } }\n";
    std::printf("STT: %s, the assistant's voice: %s\n", stt.c_str(), assistantVoice.c_str());
    char* error = nullptr;
    vasdk_agent* agent = vasdk_agent_parse(kAgent, std::strlen(kAgent), VASDK_FORMAT_TEXT, nullptr, &error);
    check(agent != nullptr, std::string("the agent parses ") + (error ? error : ""));
    if (!agent) return 1;

    // The user's voice, at the session's input rate.
    vasdk::engine::ProviderSpec phonon;
    phonon.set_id("phonon");
    va::engine::ProviderContext context;
    context.modelsDir = models.string();
    context.audio = va::audio::AudioConfig{16000, va::audio::AudioFormat::SHORT, 1};
    context.frameSize = 1600;
    std::string reason;
    auto userTts = vasdk_providers_registry(providers).create<va::audio::GenericVoiceSynthesisProvider>(phonon, context, &reason);
    check(userTts != nullptr && userTts->startVoiceSynthesisProvider(), "the user's voice (Phonon) starts " + reason);
    if (!userTts) return 1;
    UserVoice user(userTts);

    Conversation conversation;
    vasdk_session* session = vasdk_session_create(configPath.c_str(), providers, agent, nullptr, &error);
    check(session != nullptr, std::string("the offline session: VAD, STT and TTS on this machine ") + (error ? error : ""));
    if (!session) return 1;
    vasdk_session_on_event(session, onEvent, &conversation);
    check(vasdk_session_start(session, &error) == 1, "it starts");
    {
        Microphone mic(session, vasdk_session_input_rate(session));
        const auto greeting = conversation.replyPlayed(1, 90s);
        check(greeting.find("How can I help?") != std::string::npos, "the greeting, spoken: " + greeting);

        struct Line { const char* said; std::initializer_list<const char*> expect; const char* what; };
        const Line lines[] = {
            {"When are you open?", {"8 to 6"}, "the hours"},
            {"I'd like to book an oil change.", {"which day"}, "the form, the service read from the words"},
            {"Next Tuesday.", {"oil change on tuesday"}, "the day, then the read-back, the day as said"},
            {"Yes, please.", {"booked"}, "booked"},
        };
        size_t replies = 1;
        for (const auto& line : lines) {
            const auto start = std::chrono::steady_clock::now();
            mic.speak(user.say(line.said));
            const auto reply = conversation.replyPlayed(++replies, 120s);
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            check(mentions(reply, line.expect), std::string(line.what) + ": \"" + line.said + "\" heard as \"" +
                                                    conversation.lastHeard() + "\" → \"" + reply + "\" (" +
                                                    std::to_string(static_cast<int>(seconds)) + " s)");
            if (!mentions(reply, line.expect)) break;
        }
    }
    vasdk_session_free(session);
    userTts->stopVoiceSynthesisProvider();
    vasdk_agent_free(agent);
    vasdk_providers_free(providers);
    std::filesystem::remove_all(dir);
    std::printf("offline_conversation_test: %s\n", failures == 0 ? "all passed" : "failures");
    return failures == 0 ? 0 : 1;
}
