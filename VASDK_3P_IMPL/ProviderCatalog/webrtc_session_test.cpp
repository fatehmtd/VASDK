// A spoken conversation over WebRTC, as a browser has it: a C API session whose engine config
// names providers.webrtc (libdatachannel), called by a libdatachannel offerer that POSTs its offer
// to the session's signaling server, speaks the user's words (Phonon) as Opus, and reads the
// session's events from its data channel and its speech from the audio track.
// Exits 77 (skipped) when the models or a provider aren't there.
//   webrtc_session_test <models dir>                         its own session and agent, on port 18090
//   webrtc_session_test <models dir> --call <port> "<words>" calls a running app whose config names providers.webrtc

#include "ProviderCatalog.h"

#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>
#include <VASDK/network/WebRtcInterface.h>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <opus.h>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
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

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The user's voice: Phonon's audio, gathered until it's done.
class UserVoice : public va::audio::VoiceSynthesisResultListener {
public:
    explicit UserVoice(std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> tts) : _tts(std::move(tts)) {
        _tts->setVoiceSynthesisResultListener(this);
    }
    std::vector<int16_t> say(const std::string& text) {
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
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        _pcm.insert(_pcm.end(), samples, samples + size / sizeof(int16_t));
    }

private:
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> _tts;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<int16_t> _pcm;
    bool _done = false;
};

// What a browser does: an Opus sendrecv track and an "events" data channel, a 20 ms packet every
// 20 ms (silence when the user isn't speaking).
class Caller {
public:
    bool call(int port) {
        _peer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
        const uint32_t ssrc = 4242;
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(111);
        audio.addSSRC(ssrc, "caller");
        _track = _peer->addTrack(audio);
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "caller", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        _track->setMediaHandler(packetizer);
        _track->onMessage([this](rtc::binary packet) {
            // Opus speech packets are tens of bytes; the silence sent when idle, a few.
            if (packet.size() > 12 + 20 && (std::to_integer<int>(packet[1]) & 0x7F) == 111) ++speechPackets;
        }, nullptr);
        _events = _peer->createDataChannel("events");
        _events->onMessage([this](rtc::message_variant message) {
            if (const auto* text = std::get_if<std::string>(&message)) onEvent(nlohmann::json::parse(*text, nullptr, false));
        });

        auto gathered = std::make_shared<std::promise<void>>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        _peer->onGatheringStateChange([gathered, done](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete && !done->exchange(true)) gathered->set_value();
        });
        _peer->setLocalDescription();
        gathered->get_future().wait_for(10s);

        httplib::Client signaling("127.0.0.1", port);
        signaling.set_read_timeout(15, 0);
        const auto answer = signaling.Post("/offer", std::string(*_peer->localDescription()), "application/sdp");
        if (!answer || answer->status != 200) return false;
        _peer->setRemoteDescription(rtc::Description(answer->body, "answer"));
        for (int i = 0; i < 100 && !_track->isOpen(); ++i) std::this_thread::sleep_for(50ms);
        if (!_track->isOpen()) return false;
        _microphone = std::thread([this] { speakLoop(); });
        return true;
    }

    ~Caller() {
        _stop = true;
        if (_microphone.joinable()) _microphone.join();
        if (_peer) _peer->close();
    }

    void speak(const std::vector<int16_t>& pcm) {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.insert(_queue.end(), pcm.begin(), pcm.end());
    }

    // Until a final event of `type` whose text has one of `words` (lowercase) comes, from the
    // `from`th on; its text, or "".
    std::string waitFor(const std::string& type, std::initializer_list<const char*> words, std::chrono::seconds timeout,
                        size_t from = 0) {
        std::unique_lock<std::mutex> lock(_mutex);
        std::string found;
        _changed.wait_for(lock, timeout, [&] {
            for (size_t i = from; i < _finals.size(); ++i) {
                const auto& [kind, text] = _finals[i];
                if (kind == type && std::any_of(words.begin(), words.end(), [&](const char* w) { return lower(text).find(w) != std::string::npos; }))
                    return found = text, true;
            }
            return false;
        });
        return found;
    }

    size_t finals() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _finals.size();
    }

    std::atomic<int> speechPackets{0};

private:
    void onEvent(const nlohmann::json& event) {
        if (!event.is_object()) return;
        const std::string type = event.value("type", std::string());
        if ((type == "transcript" || type == "reply") && event.value("final", false)) {
            const std::string text = event.value("text", std::string());
            std::printf("  %s: %s\n", type == "reply" ? "assistant" : "heard", text.c_str());
            std::lock_guard<std::mutex> lock(_mutex);
            _finals.emplace_back(type, text);
            _changed.notify_all();
        }
    }

    void speakLoop() {
        int error = 0;
        OpusEncoder* encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
        std::vector<int16_t> frame(320);
        std::vector<unsigned char> packet(1500);
        uint32_t timestamp = 0;
        auto next = std::chrono::steady_clock::now();
        while (!_stop) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                const size_t taken = std::min(frame.size(), _queue.size());
                std::copy_n(_queue.begin(), taken, frame.begin());
                std::fill(frame.begin() + static_cast<long>(taken), frame.end(), int16_t(0));
                _queue.erase(_queue.begin(), _queue.begin() + static_cast<long>(taken));
            }
            const int size = opus_encode(encoder, frame.data(), 320, packet.data(), static_cast<int>(packet.size()));
            if (size > 0) {
                const auto* bytes = reinterpret_cast<const std::byte*>(packet.data());
                _track->sendFrame(rtc::binary(bytes, bytes + size), rtc::FrameInfo(timestamp));
            }
            timestamp += 960;   // 20 ms at Opus's 48 kHz clock
            next += 20ms;
            std::this_thread::sleep_until(next);
        }
        opus_encoder_destroy(encoder);
    }

    std::shared_ptr<rtc::PeerConnection> _peer;
    std::shared_ptr<rtc::Track> _track;
    std::shared_ptr<rtc::DataChannel> _events;
    std::thread _microphone;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<int16_t> _queue;
    std::vector<std::pair<std::string, std::string>> _finals;
};

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "hours" examples: ["when are you open", "opening hours", "what time do you open"] }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome"
      on_enter { ask { say { text: "Hello, this is the garage. How can I help?" } } }
      routes { intent: "hours" steps { say { text: "We're open from 8 to 6, Monday to Saturday." } } } } }
)";

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: webrtc_session_test <models dir> [--call <port> \"<words>\"]\n");
        return 2;
    }
    const std::filesystem::path models = std::filesystem::absolute(argv[1]);
    const bool callApp = argc > 4 && std::string(argv[2]) == "--call";
    for (const char* needed : {"silero_vad.onnx", "whistle/whistle.cact", "phonon_tts"}) {
        if (!std::filesystem::exists(models / needed)) {
            std::printf("skipped: %s isn't in %s\n", needed, models.string().c_str());
            return 77;
        }
    }
    vasdk_providers* providers = vasdk_providers_create();
    vasdk_providers_add_all(providers);
    if (vasdk_providers_registry(providers).ids<va::network::WebRtcInterface>().empty()) {
        std::printf("skipped: no WebRTC peer in this binary (VASDK_BUILD_LIBDATACHANNEL_WEBRTC)\n");
        return 77;
    }

    // The user's voice, at 16 kHz.
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

    if (callApp) {
        Caller caller;
        check(caller.call(std::atoi(argv[3])), std::string("the app answers the call on port ") + argv[3]);
        caller.waitFor("reply", {""}, 30s);   // its greeting, if it has one
        std::this_thread::sleep_for(2s);
        const size_t before = caller.finals();
        caller.speak(user.say(argv[4]));
        check(!caller.waitFor("transcript", {""}, 30s, before).empty(), "the app hears the words");
        check(!caller.waitFor("reply", {""}, 60s, before).empty(), "and answers");
        std::this_thread::sleep_for(3s);   // the rest of what it says
        check(caller.speechPackets > 0, "its speech comes back as Opus (" + std::to_string(caller.speechPackets.load()) + " packets)");
        vasdk_providers_free(providers);
        return failures == 0 ? 0 : 1;
    }

    const int port = 18090;
    const auto dir = std::filesystem::temp_directory_path() / "vasdk_webrtc_session";
    std::filesystem::create_directories(dir);
    const auto configPath = (dir / "engine.txtpb").string();
    std::ofstream(configPath) << "models_dir: \"" << models.generic_string() << "\"\n"
                              << "providers { vad { id: \"silero\" } stt { id: \"whistle\" } tts { id: \"phonon\" }\n"
                              << "  webrtc { id: \"libdatachannel\" options { [type.googleapis.com/vasdk.providers.LibDataChannelWebRtcOptions] {"
                              << " port: " << port << " } } } }\n";
    char* error = nullptr;
    vasdk_agent* agent = vasdk_agent_parse(kAgent, std::strlen(kAgent), VASDK_FORMAT_TEXT, nullptr, &error);
    check(agent != nullptr, std::string("the agent parses ") + (error ? error : ""));
    vasdk_session* session = vasdk_session_create(configPath.c_str(), providers, agent, nullptr, &error);
    check(session != nullptr, std::string("a session over WebRTC ") + (error ? error : ""));
    if (!session) return 1;
    check(vasdk_session_start(session, &error) == 1, "it starts and serves its signaling");
    {
        Caller caller;
        check(caller.call(port), "a browser's offer is answered and the call connects");
        check(!caller.waitFor("reply", {"how can i help"}, 60s).empty(), "the greeting comes as an event");
        for (int i = 0; i < 150 && caller.speechPackets <= 20; ++i) std::this_thread::sleep_for(100ms);   // slower under load
        check(caller.speechPackets > 20, "and as speech on the audio track (" + std::to_string(caller.speechPackets.load()) + " packets)");
        const size_t before = caller.finals();
        caller.speak(user.say("When are you open?"));
        check(!caller.waitFor("transcript", {"open"}, 30s, before).empty(), "the user's words, sent as Opus, are heard");
        check(!caller.waitFor("reply", {"8 to 6"}, 30s, before).empty(), "and answered");
    }
    vasdk_session_free(session);
    vasdk_agent_free(agent);
    vasdk_providers_free(providers);
    std::printf("webrtc session: %s\n", failures == 0 ? "all passed" : "failed");
    return failures == 0 ? 0 : 1;
}
