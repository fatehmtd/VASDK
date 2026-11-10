#include "Metrics.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace va::thirdparty::evaluation {

namespace {

bool inside(const std::vector<Interval>& intervals, double time) {
    for (const auto& interval : intervals)
        if (time >= interval.start && time < interval.end) return true;
    return false;
}

bool nearBoundary(const std::vector<Interval>& intervals, double time, double collar) {
    for (const auto& interval : intervals)
        if (std::fabs(time - interval.start) < collar || std::fabs(time - interval.end) < collar) return true;
    return false;
}

} // namespace

DetectionCounts& DetectionCounts::operator+=(const DetectionCounts& other) {
    truePositive += other.truePositive;
    falsePositive += other.falsePositive;
    falseNegative += other.falseNegative;
    trueNegative += other.trueNegative;
    return *this;
}

DetectionCounts scoreDetection(const std::vector<Interval>& labelled, const std::vector<Interval>& detected, double duration,
                               double step, double collar) {
    DetectionCounts counts;
    const auto frames = static_cast<long>(std::floor(duration / step));
    for (long frame = 0; frame < frames; ++frame) {
        const double time = (frame + 0.5) * step;
        if (collar > 0 && nearBoundary(labelled, time, collar)) continue;
        const bool speech = inside(labelled, time), found = inside(detected, time);
        if (speech && found) counts.truePositive += step;
        else if (found) counts.falsePositive += step;
        else if (speech) counts.falseNegative += step;
        else counts.trueNegative += step;
    }
    return counts;
}

std::vector<Interval> merged(std::vector<Interval> intervals, double gap) {
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.start < b.start; });
    std::vector<Interval> out;
    for (const auto& interval : intervals) {
        if (!out.empty() && interval.start <= out.back().end + gap) out.back().end = std::max(out.back().end, interval.end);
        else out.push_back(interval);
    }
    return out;
}

std::vector<double> onsetDelays(const std::vector<Interval>& labelled, const std::vector<Interval>& detected) {
    std::vector<double> delays;
    for (const auto& label : labelled) {
        for (const auto& found : detected) {
            if (found.end > label.start && found.start < label.end) {
                delays.push_back(found.start - label.start);
                break;
            }
        }
    }
    return delays;
}

std::vector<double> offsetDelays(const std::vector<Interval>& labelled, const std::vector<Interval>& detected) {
    std::vector<double> delays;
    for (const auto& label : labelled) {
        const Interval* last = nullptr;
        for (const auto& found : detected)
            if (found.end > label.start && found.start < label.end) last = &found;
        if (last) delays.push_back(last->end - label.end);
    }
    return delays;
}

double equalErrorRate(const std::vector<float>& same, const std::vector<float>& different, float* threshold) {
    if (same.empty() || different.empty()) return 0.0;
    std::vector<float> candidates(same);
    candidates.insert(candidates.end(), different.begin(), different.end());
    std::sort(candidates.begin(), candidates.end());
    double best = 1.0, bestGap = 2.0;
    float bestThreshold = candidates.front();
    for (const float candidate : candidates) {
        const double falseReject = static_cast<double>(std::count_if(same.begin(), same.end(), [&](float s) { return s < candidate; })) / same.size();
        const double falseAccept =
            static_cast<double>(std::count_if(different.begin(), different.end(), [&](float s) { return s >= candidate; })) / different.size();
        const double gap = std::fabs(falseReject - falseAccept);
        if (gap < bestGap) {
            bestGap = gap;
            best = (falseReject + falseAccept) / 2;
            bestThreshold = candidate;
        }
    }
    if (threshold) *threshold = bestThreshold;
    return best;
}

double accuracyAt(const std::vector<float>& same, const std::vector<float>& different, float threshold) {
    const size_t total = same.size() + different.size();
    if (total == 0) return 0.0;
    const auto right = std::count_if(same.begin(), same.end(), [&](float s) { return s >= threshold; }) +
                       std::count_if(different.begin(), different.end(), [&](float s) { return s < threshold; });
    return static_cast<double>(right) / total;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0, normA = 0, normB = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    return normA > 0 && normB > 0 ? static_cast<float>(dot / std::sqrt(normA * normB)) : 0.0f;
}

double rms(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0;
    double sum = 0;
    for (const float sample : samples) sum += static_cast<double>(sample) * sample;
    return std::sqrt(sum / samples.size());
}

std::vector<float> mixAtSnr(const std::vector<float>& speech, const std::vector<float>& noise, double snrDb) {
    std::vector<float> out(speech);
    if (noise.empty()) return out;
    std::vector<float> looped(speech.size());
    for (size_t i = 0; i < looped.size(); ++i) looped[i] = noise[i % noise.size()];
    const double noiseLevel = rms(looped);
    if (noiseLevel <= 0) return out;
    const double gain = rms(speech) / (noiseLevel * std::pow(10.0, snrDb / 20.0));
    for (size_t i = 0; i < out.size(); ++i) out[i] += static_cast<float>(looped[i] * gain);
    return out;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

double mean(const std::vector<double>& values) {
    return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

} // namespace va::thirdparty::evaluation
