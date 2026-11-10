// TTS: each FLEURS sentence of a language the voice speaks is synthesized, checked as audio (not
// silent, not clipped, a sane pace, no long silence first) and transcribed by a judge STT, whose
// error rate says how intelligible it is. The judge's own error on real speech is the floor
// (its stt scores). Synthesized speech is written to the report folder to listen to.

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/WavFile.h>
#include <VASDK/metrics/TranscriptScore.h>

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace va::thirdparty::evaluation {

namespace {

// The languages each local voice is documented to speak with its defaults (docs/providers/<id>.md)
struct Voices {
    const char* provider;
    const char* codes[2];
};
constexpr Voices kTtsLanguages[] = {
    {"pocket", {"en-US", nullptr}},
    {"phonon", {"en-US", nullptr}},       // French needs its language option: not measured yet
    {"supertonic", {"en-US", nullptr}},   // likewise
    {"moss", {"en-US", nullptr}},
};

bool speaks(const std::string& provider, const std::string& language) {
    for (const auto& entry : kTtsLanguages) {
        if (provider != entry.provider) continue;
        for (const char* code : entry.codes)
            if (code && language == code) return true;
        return false;
    }
    return language == "en-US";
}

constexpr double kSynthesisTimeout = 120.0;   // seconds for one sentence
constexpr double kSlowestSecondsPerWord = 1.2;
constexpr double kFastestSecondsPerWord = 0.15;
constexpr double kLongestLeadingSilence = 1.0;  // seconds
constexpr float kClippedLevel = 0.999f;
constexpr float kSilentPeak = 0.03f;            // about -30 dBFS

// A voice's audio, gathered until it's done
class Speaker : public va::audio::VoiceSynthesisResultListener {
public:
    struct Result {
        std::vector<short> pcm;
        double firstAudio = -1;   // seconds from the request to the first audio
        double total = 0;         // seconds from the request to completion
        std::string error;
    };

    explicit Speaker(std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> tts) : _tts(std::move(tts)) {
        _tts->setVoiceSynthesisResultListener(this);
    }
    Result say(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _result = {};
            _done = false;
            _start = now();
        }
        if (!_tts->synthesizeSpeech(text)) return {{}, -1, 0, "synthesizeSpeech refused the text"};
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_changed.wait_for(lock, std::chrono::duration<double>(kSynthesisTimeout), [this] { return _done; })) {
            _result.error = "no end of synthesis after " + std::to_string(static_cast<int>(kSynthesisTimeout)) + " s";
            lock.unlock();
            _tts->cancelSynthesis();
            lock.lock();
            _changed.wait_for(lock, std::chrono::seconds(10), [this] { return _done; });
        }
        _result.total = now() - _start;
        return _result;
    }
    void onSynthesisStarted() override {}
    void onSynthesisCompleted() override {
        std::lock_guard<std::mutex> lock(_mutex);
        _done = true;
        _changed.notify_all();
    }
    void onSynthesisError(const std::string& error) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _result.error = error;
        _done = true;
        _changed.notify_all();
    }
    void onAudioDataReceived(const char* data, size_t size) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_result.firstAudio < 0 && size > 0) _result.firstAudio = now() - _start;
        const auto* samples = reinterpret_cast<const short*>(data);
        _result.pcm.insert(_result.pcm.end(), samples, samples + size / sizeof(short));
    }

private:
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> _tts;
    std::mutex _mutex;
    std::condition_variable _changed;
    Result _result;
    bool _done = false;
    double _start = 0;
};

// Sentences as people write them: the FLEURS clips (LibriSpeech's texts have no case or punctuation)
bool written(const vasdk::eval::Clip& clip) { return clip.source().rfind("FLEURS", 0) == 0; }

} // namespace

StageResult evaluateTts(const StageContext& stage, const std::string& provider, Report& report) {
    if (!isBuilt<va::audio::GenericVoiceSynthesisProvider>(*stage.registry, provider)) return {false, "tts " + provider + " isn't built"};
    if (stage.judge.empty()) return {false, "no STT is built to judge synthesized speech"};
    std::string reason;
    auto tts = createProvider<va::audio::GenericVoiceSynthesisProvider>(stage, provider, &reason);
    if (!tts || !tts->startVoiceSynthesisProvider()) return {false, "tts " + provider + " doesn't start: " + reason};
    auto judge = createProvider<va::audio::GenericTranscriptionProvider>(stage, stage.judge, &reason);
    if (!judge || !judge->initializeTranscriptionProvider()) {
        tts->stopVoiceSynthesisProvider();
        return {false, "the judge, stt " + stage.judge + ", doesn't start: " + reason};
    }
    judge->setAudioConfiguration(stage.context.audio);
    judge->startTranscriptionProvider();
    Speaker speaker(tts);
    const int rate = stage.context.audio.sampleRate;
    std::filesystem::path folder;
    if (!stage.outputFolder.empty()) {
        folder = std::filesystem::path(stage.outputFolder) / "tts" / provider;
        std::filesystem::create_directories(folder);
    }

    bool any = false;
    for (const auto& language : stage.data->languages(vasdk::eval::SPEECH, stage.tier)) {
        if (!speaks(provider, language)) continue;
        va::metrics::EditCounts words, characters;
        std::vector<double> firstAudio;
        double audioSeconds = 0, busySeconds = 0, clippedSamples = 0, allSamples = 0;
        int clips = 0, bad = 0;
        for (const auto* clip : stage.data->clips(vasdk::eval::SPEECH, stage.tier, language)) {
            if (!written(*clip)) continue;
            const auto said = speaker.say(clip->text());
            ++clips;
            std::vector<std::string> problems;
            if (!said.error.empty()) problems.push_back(said.error);
            const double seconds = static_cast<double>(said.pcm.size()) / rate;
            float peak = 0;
            size_t clipped = 0, leading = said.pcm.size();
            bool finite = true;
            for (size_t i = 0; i < said.pcm.size(); ++i) {
                const float sample = std::fabs(said.pcm[i] / 32768.0f);
                finite = finite && std::isfinite(sample);
                peak = std::max(peak, sample);
                if (sample >= kClippedLevel) ++clipped;
                if (leading == said.pcm.size() && sample >= kSilentPeak) leading = i;
            }
            const size_t wordCount = va::metrics::transcriptWords(clip->text()).size();
            const double perWord = wordCount ? seconds / wordCount : 0;
            if (said.pcm.empty()) problems.push_back("no audio");
            else if (peak < kSilentPeak) problems.push_back("silent (peak " + formatValue(peak, Unit::Ratio) + ")");
            if (!finite) problems.push_back("samples that aren't numbers");
            if (!said.pcm.empty() && (perWord > kSlowestSecondsPerWord || perWord < kFastestSecondsPerWord))
                problems.push_back(formatValue(perWord, Unit::Seconds) + " per word");
            if (static_cast<double>(leading) / rate > kLongestLeadingSilence)
                problems.push_back(formatValue(static_cast<double>(leading) / rate, Unit::Seconds) + " of silence first");

            std::string error;
            const std::string heard = transcribeOnce(*judge, said.pcm, &error);
            if (!error.empty()) problems.push_back("the judge failed: " + error);
            const auto score = va::metrics::scoreWords(clip->text(), heard);
            words += score;
            characters += va::metrics::scoreCharacters(clip->text(), heard);
            audioSeconds += seconds;
            busySeconds += said.total;
            if (said.firstAudio >= 0) firstAudio.push_back(said.firstAudio);
            clippedSamples += static_cast<double>(clipped);
            allSamples += static_cast<double>(said.pcm.size());
            if (!problems.empty()) {
                ++bad;
                std::string line = clip->id() + ":";
                for (const auto& problem : problems) line += " " + problem + ";";
                report.note("tts", provider, line);
            } else if (score.errors() > 0) {
                report.note("tts", provider, clip->id() + " (" + std::to_string(score.errors()) + " of " + std::to_string(score.referenceLength) +
                                                 " words): said \"" + clip->text() + "\", " + stage.judge + " heard \"" + heard + "\"");
            }
            if (!folder.empty() && !said.pcm.empty()) {
                const auto samples = va::audio::fromPcm16(said.pcm.data(), said.pcm.size());
                va::audio::writeWav((folder / (clip->id() + ".wav")).string(), samples.data(), samples.size(), rate, 1);
            }
            if (stage.verbose)
                std::printf("tts %s %s: %.2f s audio in %.2f s, %s heard \"%s\"\n", provider.c_str(), clip->id().c_str(), seconds, said.total,
                            stage.judge.c_str(), heard.c_str());
        }
        if (clips == 0) continue;
        any = true;
        const std::string judged = "judge " + stage.judge;
        report.add({"tts", provider, judged, language, "judge_wer", words.errorRate(), false, true, Unit::Rate, clips});
        report.add({"tts", provider, judged, language, "judge_cer", characters.errorRate(), false, true, Unit::Rate, clips});
        report.add({"tts", provider, judged, language, "bad_clips", static_cast<double>(bad) / clips, false, true, Unit::Rate, clips});
        report.add({"tts", provider, judged, language, "clipped", allSamples > 0 ? clippedSamples / allSamples : 0, false, true, Unit::Rate, clips});
        report.add({"tts", provider, judged, language, "first_audio", median(firstAudio), false, false, Unit::Seconds, clips});
        report.add({"tts", provider, judged, language, "rtf", audioSeconds > 0 ? busySeconds / audioSeconds : 0, false, false, Unit::Ratio, clips});
    }
    tts->stopVoiceSynthesisProvider();
    judge->stopTranscriptionProvider();
    judge->releaseTranscriptionProvider();
    return any ? StageResult{true, {}} : StageResult{false, "no sentences in a language " + provider + " speaks"};
}

} // namespace va::thirdparty::evaluation
