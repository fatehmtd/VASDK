#include "nlu/EmbeddingNluProvider.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace va {
namespace nlu {

namespace {

// "book an @service for @sys.date" → "book an service for date": an entity reads as its name.
std::string withEntityNames(const std::string& example) {
    std::istringstream tokens(example);
    std::string token, text;
    while (tokens >> token) {
        if (token[0] == '@') {
            token = token.substr(token.rfind('.') == std::string::npos ? 1 : token.rfind('.') + 1);
            std::replace(token.begin(), token.end(), '_', ' ');
        }
        text += (text.empty() ? "" : " ") + token;
    }
    return text;
}

} // namespace

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    float dot = 0;
    for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;   // both have length 1
}

EmbeddingNluProvider::EmbeddingNluProvider(std::shared_ptr<const SentenceEncoder> encoder,
                                           const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents,
                                           float minSimilarity)
    : _encoder(std::move(encoder)), _minSimilarity(minSimilarity) {
    for (const auto& intent : intents) {
        std::vector<std::string> examples;
        for (const auto& example : intent.examples()) examples.push_back(withEntityNames(example));
        _intents.emplace_back(intent.name(), std::move(examples));
    }
}

const std::vector<float>& EmbeddingNluProvider::encoded(const std::string& text) const {
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        const auto found = _cache.find(text);
        if (found != _cache.end()) return found->second;
    }
    auto vector = _encoder->encode(text);   // outside the lock: encoding takes milliseconds
    std::lock_guard<std::mutex> lock(_cacheMutex);
    return _cache.emplace(text, std::move(vector)).first->second;
}

NluResult EmbeddingNluProvider::understand(const NluRequest& request) const {
    NluResult result;
    result.stage = name();
    const auto& said = encoded(request.text);
    if (said.empty()) return result;
    for (const auto& [intent, examples] : _intents) {
        if (!request.candidates.empty() &&
            std::find(request.candidates.begin(), request.candidates.end(), intent) == request.candidates.end())
            continue;
        float best = 0;
        for (const auto& example : examples) best = std::max(best, cosine(said, encoded(example)));
        if (best >= _minSimilarity) result.intents.push_back({intent, best, best});
    }
    std::stable_sort(result.intents.begin(), result.intents.end(),
                     [](const IntentMatch& a, const IntentMatch& b) { return a.score > b.score; });
    return result;
}

std::pair<int, float> EmbeddingNluProvider::closest(const std::string& sentence, const std::vector<std::string>& texts,
                                                    float minSimilarity) const {
    const auto& said = encoded(sentence);
    std::pair<int, float> best{-1, minSimilarity};
    for (size_t i = 0; i < texts.size(); ++i) {
        const float similarity = cosine(said, encoded(texts[i]));
        if (similarity >= best.second) best = {static_cast<int>(i), similarity};
    }
    return best;
}

} // namespace nlu
} // namespace va
