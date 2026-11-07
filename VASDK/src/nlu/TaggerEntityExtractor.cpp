#include "nlu/TaggerEntityExtractor.h"

#include "flow/Entities.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace va {
namespace nlu {

namespace {

struct Sentence {
    std::vector<std::string> words;
    std::vector<size_t> classes;   // per word: 0, or 1 + the entity's index
};

// What stands in for a placeholder of an entity the tagger doesn't learn (sys.date...): its words aren't one.
std::string standIn(const std::string& entity) {
    if (entity == "sys.date" || entity == "sys.datetime") return "tomorrow";
    if (entity == "sys.time") return "9 am";
    if (entity == "sys.duration") return "an hour";
    if (entity == "sys.integer" || entity == "sys.number" || entity == "sys.ordinal") return "two";
    if (entity == "sys.phone" || entity == "sys.digits") return "555 0100";
    if (entity == "sys.email") return "john@example.com";
    return "";
}

// "book @service for @sys.date": the example's words, each placeholder filled with variant `variant`.
Sentence filled(const std::string& example, size_t variant, const std::vector<EntityLabel>& labels) {
    Sentence sentence;
    size_t at = 0;
    while (at < example.size()) {
        const size_t space = example.find(' ', at);
        std::string token = example.substr(at, space == std::string::npos ? std::string::npos : space - at);
        at = space == std::string::npos ? example.size() : space + 1;
        if (token.empty()) continue;
        if (token[0] != '@') {
            for (auto& word : flow::entityWords(token)) {
                sentence.words.push_back(std::move(word));
                sentence.classes.push_back(0);
            }
            continue;
        }
        std::string entity = token.substr(1);
        while (!entity.empty() && !(std::isalnum(static_cast<unsigned char>(entity.back())) || entity.back() == '_')) entity.pop_back();
        const auto label = std::find_if(labels.begin(), labels.end(), [&](const EntityLabel& l) { return l.entity == entity; });
        const bool learned = label != labels.end() && !label->examples.empty();
        const std::string value = learned ? label->examples[variant % label->examples.size()] : standIn(entity);
        for (auto& word : flow::entityWords(value)) {
            sentence.words.push_back(std::move(word));
            sentence.classes.push_back(learned ? static_cast<size_t>(label - labels.begin()) + 1 : 0);
        }
    }
    return sentence;
}

} // namespace

TaggerEntityExtractor::TaggerEntityExtractor(std::shared_ptr<const SentenceEncoder> encoder, const std::vector<EntityLabel>& labels,
                                             const std::vector<std::string>& examples)
    : _encoder(std::move(encoder)) {
    const auto started = std::chrono::steady_clock::now();
    _classes.push_back("");
    for (const auto& label : labels) _classes.push_back(label.entity);

    std::vector<Sentence> sentences;
    for (size_t entity = 0; entity < labels.size(); ++entity)   // each value alone
        for (const auto& value : labels[entity].examples) {
            Sentence sentence{flow::entityWords(value), {}};
            sentence.classes.assign(sentence.words.size(), entity + 1);
            if (!sentence.words.empty()) sentences.push_back(std::move(sentence));
        }
    for (const auto& example : examples) {   // in context, once per value of its most varied placeholder
        size_t variants = 1;
        for (const auto& label : labels)
            if (example.find("@" + label.entity) != std::string::npos) variants = std::max(variants, label.examples.size());
        for (size_t variant = 0; variant < variants; ++variant) {
            auto sentence = filled(example, variant, labels);
            if (!sentence.words.empty()) sentences.push_back(std::move(sentence));
        }
    }
    if (!_encoder || labels.empty() || sentences.empty()) return;

    // The features of every word, encoded once.
    std::vector<std::pair<std::vector<float>, size_t>> samples;
    for (const auto& sentence : sentences) {
        const auto vectors = _encoder->encodeWords(sentence.words);
        if (vectors.size() != sentence.words.size() || vectors.empty() || vectors[0].empty()) {
            spdlog::warn("[Tagger] {} gives no word vectors: the tagger finds nothing", _encoder->name());
            return;
        }
        _dimension = vectors[0].size();
        for (size_t word = 0; word < vectors.size(); ++word) samples.emplace_back(features(vectors, word), sentence.classes[word]);
    }
    _sentences = sentences.size();

    // Softmax regression by gradient descent, in a fixed order so a start trains the same tagger.
    const size_t width = 3 * _dimension + 1, classes = _classes.size();
    _weights.assign(classes * width, 0.0f);
    constexpr int kEpochs = 40;
    constexpr float kRate = 0.2f, kEntityWeight = 3.0f;   // entity words are few: they count more
    for (int epoch = 0; epoch < kEpochs; ++epoch) {
        const float rate = kRate / (1.0f + 0.1f * static_cast<float>(epoch));
        for (const auto& [input, expected] : samples) {
            const auto probability = probabilities(input);
            const float weight = expected == 0 ? 1.0f : kEntityWeight;
            for (size_t c = 0; c < classes; ++c) {
                const float error = (probability[c] - (c == expected ? 1.0f : 0.0f)) * weight * rate;
                float* row = &_weights[c * width];
                for (size_t i = 0; i < width; ++i) row[i] -= error * input[i];
            }
        }
    }
    spdlog::info("[Tagger] trained on {} sentences ({} words, {} entities) in {} ms", _sentences, samples.size(), labels.size(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
}

std::vector<float> TaggerEntityExtractor::features(const std::vector<std::vector<float>>& vectors, size_t word) const {
    std::vector<float> input(3 * _dimension + 1, 0.0f);
    std::copy(vectors[word].begin(), vectors[word].end(), input.begin());
    if (word > 0) std::copy(vectors[word - 1].begin(), vectors[word - 1].end(), input.begin() + static_cast<long>(_dimension));
    if (word + 1 < vectors.size())
        std::copy(vectors[word + 1].begin(), vectors[word + 1].end(), input.begin() + static_cast<long>(2 * _dimension));
    input.back() = 1.0f;   // bias
    return input;
}

std::vector<float> TaggerEntityExtractor::probabilities(const std::vector<float>& input) const {
    const size_t width = 3 * _dimension + 1;
    std::vector<float> scores(_classes.size(), 0.0f);
    for (size_t c = 0; c < _classes.size(); ++c) {
        const float* row = &_weights[c * width];
        for (size_t i = 0; i < width; ++i) scores[c] += row[i] * input[i];
    }
    const float top = *std::max_element(scores.begin(), scores.end());
    float total = 0;
    for (auto& score : scores) total += (score = std::exp(score - top));
    for (auto& score : scores) score /= total;
    return scores;
}

std::vector<EntitySpan> TaggerEntityExtractor::extract(const std::vector<std::string>& words, const std::vector<EntityLabel>& labels,
                                                       bool) const {
    if (!trained() || words.empty()) return {};
    const auto vectors = _encoder->encodeWords(words);
    if (vectors.size() != words.size()) return {};
    std::vector<EntitySpan> spans;
    for (size_t word = 0; word < words.size(); ++word) {
        const auto probability = probabilities(features(vectors, word));
        const auto best = static_cast<size_t>(std::max_element(probability.begin(), probability.end()) - probability.begin());
        const bool asked = best > 0 && std::any_of(labels.begin(), labels.end(),
                                                   [&](const EntityLabel& label) { return label.entity == _classes[best]; });
        if (!asked) continue;
        if (!spans.empty() && spans.back().entity == _classes[best] && spans.back().start + spans.back().length == word) {
            auto& span = spans.back();   // the same entity goes on: one span, its score the mean
            span.score = (span.score * static_cast<float>(span.length) + probability[best]) / static_cast<float>(span.length + 1);
            ++span.length;
        } else {
            spans.push_back({_classes[best], word, 1, probability[best]});
        }
    }
    spans.erase(std::remove_if(spans.begin(), spans.end(), [](const EntitySpan& span) { return span.score < 0.5f; }), spans.end());
    return spans;
}

} // namespace nlu
} // namespace va
