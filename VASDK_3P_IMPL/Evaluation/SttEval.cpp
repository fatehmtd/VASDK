// STT: each speech clip transcribed, one-shot (writeFullAudioData, as the pipeline does when the
// VAD ends an utterance) or streaming (100 ms frames through writeAudioData, half a second of
// silence, commitEndOfSpeech, then the finals), and scored against what was said.

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/metrics/TranscriptScore.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace va::thirdparty::evaluation {

namespace {

// The languages each local recognizer is documented to know (docs/providers/<id>.md)
struct Languages {
    const char* provider;
    const char* codes[2];
};
constexpr Languages kSttLanguages[] = {
    {"whistle", {"en-US", "fr-FR"}},
    {"parakeet", {"en-US", "fr-FR"}},
    {"moonshine", {"en-US", nullptr}},
};

bool speaks(const std::string& provider, const std::string& language) {
    for (const auto& entry : kSttLanguages) {
        if (provider != entry.provider) continue;
        for (const char* code : entry.codes)
            if (code && language == code) return true;
        return false;
    }
    return language == "en-US";   // a recognizer this table doesn't list: English
}

// What a streaming recognizer says, with when
class Listener : public va::audio::TranscriptionResultListener {
public:
    void reset() {
        std::lock_guard<std::mutex> lock(_mutex);
        _finals.clear();
        _errors.clear();
        _firstPartial = _lastFinal = 0;
    }
    void onTranscriptionStarted() override {}
    void onTranscriptionStopped() override {}
    void onTranscriptionError(const std::string& error) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _errors.push_back(error);
        _changed.notify_all();
    }
    void onPartialTranscription(const va::audio::TranscriptionResult& result) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_firstPartial == 0 && !result.text.empty()) _firstPartial = now();
    }
    void onFinalTranscription(const va::audio::TranscriptionResult& result) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!result.text.empty()) _finals.push_back(result.text);
        _lastFinal = now();
        _changed.notify_all();
    }

    /// Waits for a final after `since`: up to `quiet` seconds when one came already, else up to `longest`
    void waitForFinal(double since, double quiet, double longest) {
        std::unique_lock<std::mutex> lock(_mutex);
        const bool hadOne = !_finals.empty();
        const auto newFinal = [&] { return _lastFinal >= since; };
        _changed.wait_for(lock, std::chrono::duration<double>(hadOne ? quiet : longest), newFinal);
    }
    std::string heard() const {
        std::lock_guard<std::mutex> lock(_mutex);
        std::string joined;
        for (const auto& text : _finals) joined += (joined.empty() ? "" : " ") + text;
        return joined;
    }
    double firstPartial() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _firstPartial;
    }
    double lastFinal() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _lastFinal;
    }
    std::vector<std::string> errors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<std::string> _finals;
    std::vector<std::string> _errors;
    double _firstPartial = 0;
    double _lastFinal = 0;
};

std::string streamClip(va::audio::GenericTranscriptionProvider& recognizer, Listener& listener, const std::vector<short>& pcm,
                       const StageContext& stage, double& firstPartial, double& finalize) {
    listener.reset();
    const size_t frame = static_cast<size_t>(stage.context.frameSize);
    const double start = now();
    std::vector<short> padded(pcm);
    padded.resize(pcm.size() + kSampleRate / 2, 0);   // half a second of silence, as before a VAD's end
    for (size_t at = 0; at < padded.size(); at += frame) {
        const size_t count = std::min(frame, padded.size() - at);
        recognizer.writeAudioData(reinterpret_cast<const uint8_t*>(padded.data() + at), count * sizeof(short));
        if (stage.realtime) {
            const double due = start + static_cast<double>(at + count) / kSampleRate;
            const double wait = due - now();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
    }
    const double committed = now();
    recognizer.commitEndOfSpeech();
    listener.waitForFinal(committed, 2.0, 30.0);
    firstPartial = listener.firstPartial() > 0 ? listener.firstPartial() - start : -1;
    finalize = listener.lastFinal() >= committed ? listener.lastFinal() - committed : 0;
    return listener.heard();
}

} // namespace

StageResult evaluateStt(const StageContext& stage, const std::string& provider, const std::string& mode, Report& report) {
    if (!isBuilt<va::audio::GenericTranscriptionProvider>(*stage.registry, provider)) return {false, "stt " + provider + " isn't built"};
    std::vector<std::string> languages;
    for (const auto& language : stage.data->languages(vasdk::eval::SPEECH, stage.tier))
        if (speaks(provider, language)) languages.push_back(language);
    if (languages.empty()) return {false, "no speech clips in a language " + provider + " knows"};

    std::string reason;
    auto recognizer = createProvider<va::audio::GenericTranscriptionProvider>(stage, provider, &reason);
    if (!recognizer || !recognizer->initializeTranscriptionProvider()) return {false, "stt " + provider + " doesn't start: " + reason};
    recognizer->setAudioConfiguration(stage.context.audio);
    Listener listener;
    recognizer->setTranscriptionResultListener(&listener);
    if (!recognizer->startTranscriptionProvider()) return {false, "stt " + provider + " doesn't start"};

    const bool streaming = mode == "streaming";
    for (const auto& language : languages) {
        va::metrics::EditCounts words, characters;
        double audioSeconds = 0, busySeconds = 0;
        std::vector<double> firstPartials, finalizes;
        int clips = 0;
        for (const auto* clip : stage.data->clips(vasdk::eval::SPEECH, stage.tier, language)) {
            std::vector<short> pcm;
            if (!stage.data->audio(*clip, pcm, &reason)) {
                recognizer->stopTranscriptionProvider();
                return {false, reason};
            }
            const double start = now();
            std::string heard;
            if (streaming) {
                double firstPartial = -1, finalize = 0;
                heard = streamClip(*recognizer, listener, pcm, stage, firstPartial, finalize);
                if (firstPartial >= 0) firstPartials.push_back(firstPartial);
                finalizes.push_back(finalize);
                for (const auto& error : listener.errors()) report.note("stt", provider, clip->id() + ": error " + error);
            } else {
                std::string error;
                heard = transcribeOnce(*recognizer, pcm, &error);
                if (!error.empty()) {
                    recognizer->stopTranscriptionProvider();
                    recognizer->releaseTranscriptionProvider();
                    return {false, provider + " has no one-shot transcription: " + error};
                }
            }
            busySeconds += now() - start;
            audioSeconds += static_cast<double>(pcm.size()) / kSampleRate;
            const auto score = va::metrics::scoreWords(clip->text(), heard);
            words += score;
            characters += va::metrics::scoreCharacters(clip->text(), heard);
            ++clips;
            if (score.errors() > 0)
                report.note("stt", provider, mode + " " + clip->id() + " (" + std::to_string(score.errors()) + " of " +
                                                 std::to_string(score.referenceLength) + " words): said \"" + clip->text() +
                                                 "\", heard \"" + heard + "\"");
            if (stage.verbose)
                std::printf("stt %s %s %s: %zu/%zu \"%s\"\n", provider.c_str(), mode.c_str(), clip->id().c_str(), score.errors(),
                            score.referenceLength, heard.c_str());
        }
        report.add({"stt", provider, mode, language, "wer", words.errorRate(), false, true, Unit::Rate, clips});
        report.add({"stt", provider, mode, language, "cer", characters.errorRate(), false, true, Unit::Rate, clips});
        report.add({"stt", provider, mode, language, "rtf", audioSeconds > 0 ? busySeconds / audioSeconds : 0, false, false, Unit::Ratio, clips});
        if (streaming && stage.realtime) {
            report.add({"stt", provider, mode, language, "first_partial", median(firstPartials), false, false, Unit::Seconds, clips});
            report.add({"stt", provider, mode, language, "finalize", median(finalizes), false, false, Unit::Seconds, clips});
        }
    }
    recognizer->stopTranscriptionProvider();
    recognizer->releaseTranscriptionProvider();
    return {true, {}};
}

} // namespace va::thirdparty::evaluation
