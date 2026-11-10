// VAD: each composed clip fed as the pipeline feeds it, one 100 ms frame per call, and the regions
// the provider reports compared with where the speech is (10 ms grid, 100 ms collar). Noise-only
// clips (not babble, which is talk) measure false alarms. This is the provider's own output: the
// VAD processor's pause and end timers come after it.

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/VADProvider.h>

#include <cstdio>

namespace va::thirdparty::evaluation {

namespace {

std::vector<Interval> detect(const va::audio::VADProvider& vad, const std::vector<short>& pcm, size_t frame) {
    std::vector<Interval> found;
    for (size_t at = 0; at + frame <= pcm.size(); at += frame) {
        const auto regions = vad.detectVoiceActivity(reinterpret_cast<const char*>(pcm.data() + at), 0, frame);
        for (const auto& region : regions)
            found.push_back({static_cast<double>(at + region.offset) / kSampleRate,
                             static_cast<double>(at + region.offset + region.length) / kSampleRate});
    }
    return merged(found);
}

} // namespace

StageResult evaluateVad(const StageContext& stage, const std::string& provider, Report& report) {
    if (!isBuilt<va::audio::VADProvider>(*stage.registry, provider)) return {false, "vad " + provider + " isn't built"};
    std::string reason;
    auto vad = createProvider<va::audio::VADProvider>(stage, provider, &reason);
    if (!vad) return {false, "vad " + provider + " doesn't start: " + reason};
    const auto frame = static_cast<size_t>(stage.context.frameSize);

    DetectionCounts total;
    std::vector<double> onsets, offsets;
    int clips = 0;
    for (const auto* clip : stage.data->clips(vasdk::eval::VAD, stage.tier)) {
        std::vector<short> pcm;
        if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
        std::vector<Interval> labelled;
        for (const auto& segment : clip->speech()) labelled.push_back({segment.start_ms() / 1000.0, segment.end_ms() / 1000.0});
        const auto found = detect(*vad, pcm, frame);
        const auto counts = scoreDetection(labelled, found, static_cast<double>(pcm.size()) / kSampleRate);
        total += counts;
        for (const double delay : onsetDelays(labelled, found)) onsets.push_back(delay);
        for (const double delay : offsetDelays(labelled, found)) offsets.push_back(delay);
        ++clips;
        report.note("vad", provider, clip->id() + ": precision " + formatValue(counts.precision(), Unit::Rate) + ", recall " +
                                         formatValue(counts.recall(), Unit::Rate));
        if (stage.verbose) std::printf("vad %s %s: F1 %.3f\n", provider.c_str(), clip->id().c_str(), counts.f1());
    }
    double noiseSeconds = 0, falseSeconds = 0;
    int noises = 0;
    for (const auto* clip : stage.data->clips(vasdk::eval::NOISE, stage.tier)) {
        if (clip->noise() == "babble") continue;   // talkers: speech, for a VAD
        std::vector<short> pcm;
        if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
        double detected = 0;
        for (const auto& interval : detect(*vad, pcm, frame)) detected += interval.end - interval.start;
        noiseSeconds += static_cast<double>(pcm.size()) / kSampleRate;
        falseSeconds += detected;
        ++noises;
        if (detected > 0) report.note("vad", provider, clip->id() + ": " + formatValue(detected, Unit::Seconds) + " taken for speech");
    }
    if (clips == 0) return {false, "no VAD clips in the set"};
    report.add({"vad", provider, "", "", "precision", total.precision(), true, false, Unit::Rate, clips});
    report.add({"vad", provider, "", "", "recall", total.recall(), true, false, Unit::Rate, clips});
    report.add({"vad", provider, "", "", "f1", total.f1(), true, true, Unit::Rate, clips});
    report.add({"vad", provider, "", "", "false_alarm", noiseSeconds > 0 ? falseSeconds / noiseSeconds : 0, false, true, Unit::Rate, noises});
    report.add({"vad", provider, "", "", "onset_delay", median(onsets), false, false, Unit::Seconds, clips});
    report.add({"vad", provider, "", "", "offset_delay", median(offsets), false, false, Unit::Seconds, clips});
    vad->release();
    return {true, {}};
}

} // namespace va::thirdparty::evaluation
