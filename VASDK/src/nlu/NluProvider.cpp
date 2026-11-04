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
        // Sure, and of enough of the words: a keyword may stand for a short sentence ("let me talk to a person",
        // up to 6 words a matched word), but a keyword in a long question goes on to the next stage.
        auto sureness = [](const NluResult& of) {
            return of.intents.empty() ? 0.0f : of.intents.front().confidence * std::min(1.0f, 6.0f * of.intents.front().explains);
        };
        if (sureness(result) >= _minConfidence) return result;
        if (result.noneMeant) return result;   // "none of them" beats an earlier stage's weak guess
        const float confidence = sureness(result);
        const float bestConfidence = sureness(best);
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

std::string phoneticKey(const std::string& word) {
    std::string text;
    for (const unsigned char c : word)
        if (std::isalpha(c)) text += static_cast<char>(std::tolower(c));
    if (text.empty()) return text;
    // Spellings that sound the same, first: "ph" is "f", "ck" is "k", a silent start ("kn", "wr").
    auto replaceAll = [&text](const std::string& from, const std::string& to) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    };
    if (text.rfind("kn", 0) == 0 || text.rfind("gn", 0) == 0 || text.rfind("wr", 0) == 0) text.erase(0, 1);
    replaceAll("ph", "f");
    replaceAll("ck", "k");
    replaceAll("th", "0");
    replaceAll("sh", "x");
    replaceAll("gh", "");
    std::string key;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == 'c') c = next == 'e' || next == 'i' || next == 'y' ? 's' : 'k';
        else if (c == 'q') c = 'k';
        else if (c == 'z') c = 's';
        else if (c == 'v') c = 'f';
        else if (c == 'x' && i > 0) c = 's';
        else if (c == 'h' && i > 0) continue;   // "john": the h is silent after a consonant or vowel
        const bool vowel = std::string("aeiouy").find(c) != std::string::npos;
        if (vowel && !key.empty()) continue;   // vowels count only at the start
        if (vowel) c = 'a';
        if (!key.empty() && key.back() == c) continue;
        key += c;
    }
    return key;
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
