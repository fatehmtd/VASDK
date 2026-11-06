// An NLU eval set as the agent hears it: each case is spoken by local voices (Phonon's, one after
// another) and recognized by a local recognizer (Whistle, else Parakeet), and the set is written
// again with the words as heard, for `vasdk_agentc eval <agent> <heard set> --sweep ...`: the
// understanding thresholds measured on recognized speech, with no network and no recordings.
//
//   vasdk_spoken_eval <models dir> <eval set.txtpb> <heard set.txtpb> [voice,voice,...] [stt]
//
// Synthetic voices are cleaner than callers on a phone: a floor for what speech costs, not a
// replacement for VASDK_Pipeline --record-turns.

#include "ProviderCatalog.h"

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/c/vasdk.h>
#include <VASDK/c/vasdk_cpp.h>
#include "vasdk/engine/engine_config.pb.h"
#include "vasdk/flow/test_case.pb.h"

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

// A voice whose audio is gathered until it's done.
class Speaker : public va::audio::VoiceSynthesisResultListener {
public:
    explicit Speaker(std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> tts) : _tts(std::move(tts)) {
        _tts->setVoiceSynthesisResultListener(this);
    }
    std::vector<short> say(const std::string& text, const std::string& voice) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pcm.clear();
            _done = false;
        }
        _tts->synthesizeStyledSpeech(text, {voice, 0});
        std::unique_lock<std::mutex> lock(_mutex);
        _changed.wait_for(lock, std::chrono::seconds(60), [this] { return _done; });
        return _pcm;
    }
    void onSynthesisStarted() override {}
    void onSynthesisCompleted() override {
        std::lock_guard<std::mutex> lock(_mutex);
        _done = true;
        _changed.notify_all();
    }
    void onSynthesisError(const std::string& error) override {
        std::fprintf(stderr, "voice: %s\n", error.c_str());
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

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    for (std::string part; std::getline(stream, part, ',');)
        if (!part.empty()) parts.push_back(part);
    return parts;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: vasdk_spoken_eval <models dir> <eval set.txtpb> <heard set.txtpb> [voice,voice,...] [stt]\n");
        return 2;
    }
    const std::filesystem::path models = std::filesystem::absolute(argv[1]);
    vasdk::flow::NluEvalSet cases;
    {
        std::ifstream in(argv[2]);
        std::stringstream text;
        text << in.rdbuf();
        if (!in || !google::protobuf::TextFormat::ParseFromString(text.str(), &cases)) {
            std::fprintf(stderr, "%s isn't an NluEvalSet\n", argv[2]);
            return 1;
        }
    }
    std::vector<std::string> voices = argc > 4 ? split(argv[4]) : std::vector<std::string>{"Anton", "Zoey", "Marcos"};

    vasdk_providers* providers = vasdk_providers_create();
    vasdk_providers_add_all(providers);
    auto& registry = vasdk_providers_registry(providers);
    const auto recognizers = registry.ids<va::audio::GenericTranscriptionProvider>();
    auto built = [&](const std::string& id) { return std::find(recognizers.begin(), recognizers.end(), id) != recognizers.end(); };
    const std::string stt = argc > 5 ? argv[5] : built("whistle") ? "whistle" : "parakeet";

    va::engine::ProviderContext context;
    context.modelsDir = models.string();
    context.audio = va::audio::AudioConfig{16000, va::audio::AudioFormat::SHORT, 1};
    context.frameSize = 1600;
    std::string reason;
    vasdk::engine::ProviderSpec voiceSpec;
    voiceSpec.set_id("phonon");
    auto tts = registry.create<va::audio::GenericVoiceSynthesisProvider>(voiceSpec, context, &reason);
    if (!tts || !tts->startVoiceSynthesisProvider()) {
        std::fprintf(stderr, "the voices (Phonon) don't start: %s\n", reason.c_str());
        return 1;
    }
    vasdk::engine::ProviderSpec sttSpec;
    sttSpec.set_id(stt);
    auto recognizer = registry.create<va::audio::GenericTranscriptionProvider>(sttSpec, context, &reason);
    if (!recognizer || !recognizer->initializeTranscriptionProvider()) {
        std::fprintf(stderr, "the recognizer (%s) doesn't start: %s\n", stt.c_str(), reason.c_str());
        return 1;
    }
    recognizer->setAudioConfiguration(context.audio);
    Speaker speaker(tts);

    std::ofstream out(argv[3]);
    out << "# proto-file: vasdk/flow/test_case.proto\n# proto-message: vasdk.flow.NluEvalSet\n#\n"
        << "# " << argv[2] << " as " << stt << " heard it, said by Phonon's " ;
    for (size_t v = 0; v < voices.size(); ++v) out << (v ? ", " : "") << voices[v];
    out << " (vasdk_spoken_eval).\n";
    int exact = 0, total = 0;
    for (const auto& voice : voices) {
        for (const auto& original : cases.cases()) {
            const auto pcm = speaker.say(original.text(), voice);
            std::string heard;
            if (!pcm.empty()) {
                try {
                    heard = recognizer->writeFullAudioData(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(short)).text;
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "%s: %s\n", stt.c_str(), error.what());
                }
            }
            auto spoken = original;
            spoken.set_text(heard);
            ++total;
            auto words = [](std::string text) {   // compared without case or punctuation
                text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::ispunct(c) && c != '\''; }), text.end());
                return lowered(text);
            };
            const bool same = words(heard) == words(original.text());
            exact += same;
            std::string line;
            google::protobuf::TextFormat::Printer printer;
            printer.SetSingleLineMode(true);
            printer.PrintToString(spoken, &line);
            out << "# " << voice << " said: " << original.text() << "\n" << "cases { " << line << "}\n";
            std::printf("%-8s %s \"%s\" → \"%s\"\n", voice.c_str(), same ? "  " : "≠ ", original.text().c_str(), heard.c_str());
        }
    }
    std::printf("%d of %d heard word for word (%s, voices %zu)\n", exact, total, stt.c_str(), voices.size());
    tts->stopVoiceSynthesisProvider();
    recognizer->releaseTranscriptionProvider();
    vasdk_providers_free(providers);
    return 0;
}
