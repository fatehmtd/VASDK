// Speaker identification: an embedding of each speech clip whose speaker has two or more clips,
// cosine similarity over every pair, and the equal error rate between same-speaker and
// different-speaker pairs; plus how pairs fare at SpeakerIdentificationProcessor's threshold.

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/SpeakerEmbeddingProvider.h>
#include <VASDK/WavFile.h>

#include <cstdio>
#include <map>

namespace va::thirdparty::evaluation {

namespace {

constexpr float kProcessorThreshold = 0.3f;   // SpeakerIdentificationProcessor's default threshold

} // namespace

StageResult evaluateSpeaker(const StageContext& stage, const std::string& provider, Report& report) {
    if (!isBuilt<va::audio::SpeakerEmbeddingProvider>(*stage.registry, provider)) return {false, "speaker " + provider + " isn't built"};
    std::string reason;
    auto embedder = createProvider<va::audio::SpeakerEmbeddingProvider>(stage, provider, &reason);
    if (!embedder) return {false, "speaker " + provider + " doesn't start: " + reason};

    std::map<std::string, int> perSpeaker;
    const auto speech = stage.data->clips(vasdk::eval::SPEECH, stage.tier);
    for (const auto* clip : speech)
        if (!clip->speaker().empty()) ++perSpeaker[clip->speaker()];
    struct Voice {
        std::string speaker, id;
        std::vector<float> embedding;
    };
    std::vector<Voice> voices;
    for (const auto* clip : speech) {
        if (perSpeaker[clip->speaker()] < 2) continue;
        std::vector<short> pcm;
        if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
        const auto samples = va::audio::fromPcm16(pcm.data(), pcm.size());
        auto embedding = embedder->computeEmbedding(samples.data(), samples.size());
        if (embedding.empty()) {
            report.note("speaker", provider, clip->id() + ": no embedding");
            continue;
        }
        voices.push_back({clip->speaker(), clip->id(), std::move(embedding)});
    }
    std::vector<float> same, different;
    for (size_t i = 0; i < voices.size(); ++i) {
        for (size_t j = i + 1; j < voices.size(); ++j) {
            const float similarity = cosine(voices[i].embedding, voices[j].embedding);
            (voices[i].speaker == voices[j].speaker ? same : different).push_back(similarity);
            if (stage.verbose)
                std::printf("speaker %s %s / %s: %.3f\n", provider.c_str(), voices[i].id.c_str(), voices[j].id.c_str(), similarity);
        }
    }
    if (same.empty() || different.empty()) return {false, "too few speakers with two clips or more in the set"};
    float threshold = 0;
    const double eer = equalErrorRate(same, different, &threshold);
    const auto clips = static_cast<int>(voices.size());
    report.add({"speaker", provider, "", "", "eer", eer, false, true, Unit::Rate, clips});
    report.add({"speaker", provider, "", "", "eer_threshold", threshold, true, false, Unit::Ratio, clips});
    const double sameAccepted = accuracyAt(same, {}, kProcessorThreshold);
    const double differentRejected = accuracyAt({}, different, kProcessorThreshold);
    report.add({"speaker", provider, "", "", "same_accepted", sameAccepted, true, false, Unit::Rate, static_cast<int>(same.size())});
    report.add({"speaker", provider, "", "", "different_rejected", differentRejected, true, false, Unit::Rate, static_cast<int>(different.size())});
    report.note("speaker", provider, std::to_string(same.size()) + " same-speaker and " + std::to_string(different.size()) +
                                         " different-speaker pairs; mean similarity " + formatValue(mean({same.begin(), same.end()}), Unit::Ratio) +
                                         " and " + formatValue(mean({different.begin(), different.end()}), Unit::Ratio));
    return {true, {}};
}

} // namespace va::thirdparty::evaluation
