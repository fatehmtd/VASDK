// The evaluation's own arithmetic, on made-up numbers (no models, no data): speech detection on a
// grid with a collar, onset and offset delays, the equal error rate, mixing at a SNR, and baselines
// that fail a worse score, pass a better one and keep a margin edited by hand.

#include "../Metrics.h"
#include "../Report.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace va::thirdparty::evaluation;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}
bool near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

} // namespace

int main() {
    // Detection: speech 1-3 s of 5 s; detected 1.5-3.5 s. No collar: TP 1.5, FP 0.5, FN 0.5, TN 2.5.
    {
        const auto counts = scoreDetection({{1.0, 3.0}}, {{1.5, 3.5}}, 5.0, 0.01, 0.0);
        check(near(counts.truePositive, 1.5, 0.011) && near(counts.falsePositive, 0.5, 0.011) && near(counts.falseNegative, 0.5, 0.011) &&
                  near(counts.trueNegative, 2.5, 0.011),
              "detection counts");
        check(near(counts.precision(), 0.75, 0.01) && near(counts.recall(), 0.75, 0.01) && near(counts.f1(), 0.75, 0.01), "precision, recall, F1");
        check(near(counts.falseAlarmRate(), 0.5 / 3.0, 0.01), "false alarm rate");
        // A 100 ms collar forgives a detection 80 ms late
        const auto forgiving = scoreDetection({{1.0, 3.0}}, {{1.08, 3.0}}, 5.0);
        check(forgiving.falseNegative == 0 && forgiving.falsePositive == 0, "the collar forgives small offsets");
        DetectionCounts total = counts;
        total += counts;
        check(near(total.truePositive, 2 * counts.truePositive), "counts add up");
        const auto nothing = scoreDetection({}, {}, 2.0);
        check(nothing.falseAlarmRate() == 0 && nothing.f1() == 1.0 && nothing.precision() == 1.0, "silence detected as silence: perfect");
    }
    // Intervals and delays
    {
        const auto joined = merged({{2.0, 3.0}, {0.0, 1.0}, {0.9, 1.5}, {3.05, 4.0}}, 0.1);
        check(joined.size() == 2 && near(joined[0].end, 1.5) && near(joined[1].start, 2.0) && near(joined[1].end, 4.0), "merged with a gap");
        const auto onsets = onsetDelays({{1.0, 2.0}, {5.0, 6.0}, {8.0, 9.0}}, {{1.1, 2.3}, {4.9, 5.5}, {5.6, 6.2}});
        check(onsets.size() == 2 && near(onsets[0], 0.1) && near(onsets[1], -0.1), "onset delays, a miss left out");
        const auto offsets = offsetDelays({{1.0, 2.0}, {5.0, 6.0}}, {{1.1, 2.3}, {4.9, 5.5}, {5.6, 6.2}});
        check(offsets.size() == 2 && near(offsets[0], 0.3) && near(offsets[1], 0.2), "offset delays from the last overlapping detection");
        check(near(median({3, 1, 2}), 2) && near(median({4, 1, 2, 3}), 2.5) && median({}) == 0, "median");
    }
    // Equal error rate: separable scores give 0; overlapping ones what they share
    {
        float threshold = 0;
        check(equalErrorRate({0.8f, 0.9f, 0.7f}, {0.1f, 0.2f, 0.3f}, &threshold) == 0.0 && threshold > 0.3f && threshold <= 0.7f,
              "separable: EER 0 at a threshold between");
        const double eer = equalErrorRate({0.9f, 0.8f, 0.7f, 0.2f}, {0.1f, 0.3f, 0.75f, 0.0f});
        check(near(eer, 0.25), "one of four on each side: EER 25%");
        check(near(accuracyAt({0.9f, 0.2f}, {0.1f, 0.5f}, 0.3f), 0.5), "accuracy at a threshold");
        check(near(cosine({1, 0}, {0, 1}), 0) && near(cosine({1, 2}, {2, 4}), 1, 1e-6) && cosine({}, {}) == 0, "cosine");
    }
    // Mixing at a SNR: the added noise has the speech's power over 10^(snr/10)
    {
        std::vector<float> speech(16000), noise(1000);
        for (size_t i = 0; i < speech.size(); ++i) speech[i] = 0.5f * std::sin(0.05f * i);
        for (size_t i = 0; i < noise.size(); ++i) noise[i] = (i % 2 ? 0.1f : -0.1f);
        const auto mixed = mixAtSnr(speech, noise, 10.0);
        std::vector<float> added(speech.size());
        for (size_t i = 0; i < speech.size(); ++i) added[i] = mixed[i] - speech[i];
        check(mixed.size() == speech.size(), "mixed keeps the speech's length (noise repeats)");
        check(near(20 * std::log10(rms(speech) / rms(added)), 10.0, 0.01), "10 dB SNR");
    }
    // Baselines
    {
        std::vector<Score> scores = {{"stt", "whistle", "oneshot", "en-US", "wer", 0.08, false, true, Unit::Rate, 10},
                                     {"vad", "silero", "", "", "f1", 0.93, true, true, Unit::Rate, 3},
                                     {"stt", "whistle", "oneshot", "en-US", "rtf", 0.2, false, false, Unit::Ratio, 10}};
        vasdk::eval::Baselines baselines;
        updateBaselines(baselines, scores, vasdk::eval::SMOKE);
        check(baselines.baselines_size() == 2, "only gated scores become baselines");
        auto result = checkBaselines(scores, baselines, vasdk::eval::SMOKE);
        check(result.checked == 2 && result.failures.empty(), "a run passes its own baselines");
        check(checkBaselines(scores, baselines, vasdk::eval::FULL).unchecked.size() == 2, "another tier has no baselines");

        auto worse = scores;
        worse[0].value = 0.08 + defaultMargin("stt", "wer") + 0.01;   // past the margin
        worse[1].value = 0.95;                                        // better
        result = checkBaselines(worse, baselines, vasdk::eval::SMOKE);
        check(result.failures.size() == 1 && result.failures[0].find("stt/whistle/oneshot/en-US wer") == 0, "a worse WER fails, a better F1 passes");
        worse[0].value = 0.08 + defaultMargin("stt", "wer") - 0.001;
        check(checkBaselines(worse, baselines, vasdk::eval::SMOKE).failures.empty(), "within the margin passes");
        worse[1].value = 0.93 - defaultMargin("vad", "f1") - 0.01;
        check(checkBaselines(worse, baselines, vasdk::eval::SMOKE).failures.size() == 1, "a lower F1 past the margin fails");

        baselines.mutable_baselines(0)->set_margin(0.5);   // edited by hand
        scores[0].value = 0.07;
        updateBaselines(baselines, scores, vasdk::eval::SMOKE);
        bool kept = false;
        for (const auto& baseline : baselines.baselines())
            if (baseline.metric() == "wer") kept = near(baseline.value(), 0.07) && near(baseline.margin(), 0.5);
        check(kept, "a new value keeps the margin edited by hand");
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("eval_metrics: all passed\n");
    return 0;
}
