#pragma once

// The measures the evaluation computes besides word error rates (va::metrics::TranscriptScore):
// speech detection against labelled segments, equal error rate, mixing at a signal-to-noise
// ratio. Plain functions over plain data, tested by tests/eval_metrics_test.cpp.

#include <cstddef>
#include <vector>

namespace va::thirdparty::evaluation {

struct Interval {
    double start = 0;   // seconds
    double end = 0;
};

/// Frames of speech against labels, on a grid. Frames within `collar` of a labelled boundary are
/// not counted: where speech starts and stops is a matter of a few tens of ms either way.
struct DetectionCounts {
    double truePositive = 0;    // seconds labelled speech and detected
    double falsePositive = 0;   // detected, labelled silence
    double falseNegative = 0;   // labelled speech, missed
    double trueNegative = 0;

    double precision() const { return truePositive + falsePositive > 0 ? truePositive / (truePositive + falsePositive) : 1.0; }
    double recall() const { return truePositive + falseNegative > 0 ? truePositive / (truePositive + falseNegative) : 1.0; }
    double f1() const {
        const double p = precision(), r = recall();
        return p + r > 0 ? 2 * p * r / (p + r) : 0.0;
    }
    /// Share of labelled silence detected as speech
    double falseAlarmRate() const { return falsePositive + trueNegative > 0 ? falsePositive / (falsePositive + trueNegative) : 0.0; }
    DetectionCounts& operator+=(const DetectionCounts& other);
};

DetectionCounts scoreDetection(const std::vector<Interval>& labelled, const std::vector<Interval>& detected, double duration,
                               double step = 0.01, double collar = 0.1);

/// Joins intervals that touch or overlap (gaps up to `gap` seconds), sorted
std::vector<Interval> merged(std::vector<Interval> intervals, double gap = 0.0);

/// Seconds from each labelled interval's start to the first detection overlapping it (missed ones are left out)
std::vector<double> onsetDelays(const std::vector<Interval>& labelled, const std::vector<Interval>& detected);
/// Seconds from each labelled interval's end to the end of the detection overlapping it last (missed ones are left out)
std::vector<double> offsetDelays(const std::vector<Interval>& labelled, const std::vector<Interval>& detected);

/// The rate at which same-source pairs are rejected equals the rate at which different-source ones
/// are accepted, at some threshold on the score (higher: more alike). `threshold`: where.
double equalErrorRate(const std::vector<float>& same, const std::vector<float>& different, float* threshold = nullptr);

/// Share of pairs judged right at a threshold: same at or above it, different below
double accuracyAt(const std::vector<float>& same, const std::vector<float>& different, float threshold);

/// Cosine similarity; 0 when either is empty or silent
float cosine(const std::vector<float>& a, const std::vector<float>& b);

double rms(const std::vector<float>& samples);
/// speech + noise scaled so the ratio of their powers is snrDb; noise repeats when shorter
std::vector<float> mixAtSnr(const std::vector<float>& speech, const std::vector<float>& noise, double snrDb);

double median(std::vector<double> values);
double mean(const std::vector<double>& values);

} // namespace va::thirdparty::evaluation
