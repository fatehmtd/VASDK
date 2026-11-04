#include "nlu/NluProvider.h"

#include <algorithm>
#include <cctype>

namespace va {
namespace nlu {

NluResult CascadeNluProvider::understand(const NluRequest& request) const {
    NluResult best;
    for (const auto& stage : _stages) {
        auto result = stage->understand(request);
        if (result.stage.empty()) result.stage = stage->name();
        if (!result.intents.empty() && result.intents.front().confidence >= _minConfidence) return result;
        const float confidence = result.intents.empty() ? 0.0f : result.intents.front().confidence;
        const float bestConfidence = best.intents.empty() ? 0.0f : best.intents.front().confidence;
        if (best.stage.empty() || confidence > bestConfidence) best = std::move(result);
    }
    return best;
}

float wordSimilarity(const std::string& a, const std::string& b) {
    if (a == b) return 1.0f;
    if (a.empty() || b.empty()) return 0.0f;
    // Damerau-Levenshtein (optimal string alignment): insert, delete, substitute, swap neighbours.
    std::vector<std::vector<size_t>> distance(a.size() + 1, std::vector<size_t>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); ++i) distance[i][0] = i;
    for (size_t j = 0; j <= b.size(); ++j) distance[0][j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            distance[i][j] = std::min({distance[i - 1][j] + 1, distance[i][j - 1] + 1, distance[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                distance[i][j] = std::min(distance[i][j], distance[i - 2][j - 2] + 1);
        }
    }
    return 1.0f - static_cast<float>(distance[a.size()][b.size()]) / static_cast<float>(std::max(a.size(), b.size()));
}

float wordMatch(const std::string& heard, const std::string& expected, float minSimilarity) {
    if (heard == expected) return 1.0f;
    if (heard.size() < 4 || expected.size() < 4 || minSimilarity >= 1.0f) return 0.0f;   // short words: "book" isn't "look"
    auto lettersOnly = [](const std::string& word) {   // "11:00" isn't "14:00", nor "2021" "2026"
        return std::none_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (!lettersOnly(heard) || !lettersOnly(expected)) return 0.0f;
    const float similarity = wordSimilarity(heard, expected);
    return similarity >= minSimilarity ? similarity : 0.0f;
}

std::vector<std::pair<size_t, float>> findPhrase(const std::vector<std::string>& words, const std::vector<std::string>& phrase,
                                                 float minSimilarity) {
    std::vector<std::pair<size_t, float>> found;
    if (phrase.empty() || phrase.size() > words.size()) return found;
    for (size_t start = 0; start + phrase.size() <= words.size(); ++start) {
        float total = 0;
        size_t i = 0;
        for (; i < phrase.size(); ++i) {
            const float match = wordMatch(words[start + i], phrase[i], minSimilarity);
            if (match == 0.0f) break;
            total += match;
        }
        if (i == phrase.size()) found.emplace_back(start, total);
    }
    return found;
}

} // namespace nlu
} // namespace va
