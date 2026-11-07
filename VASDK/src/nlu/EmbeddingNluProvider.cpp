#include "nlu/EmbeddingNluProvider.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
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

size_t EmbeddingNluProvider::precompute(const std::vector<std::string>& more) const {
    std::vector<const std::string*> texts;
    for (const auto& [intent, examples] : _intents)
        for (const auto& example : examples) texts.push_back(&example);
    for (const auto& text : more) texts.push_back(&text);
    size_t encodedNow = 0;
    for (const auto* text : texts) {
        {
            std::lock_guard<std::mutex> lock(_cacheMutex);
            if (_cache.count(*text)) continue;
        }
        encoded(*text);
        ++encodedNow;
    }
    return encodedNow;
}

// The cache file: "VASDKEMB", version 1, the encoder's name, the vector size, then each text
// with its vector. Little-endian, as written on the device that reads it.
bool EmbeddingNluProvider::saveCache(const std::string& path, std::string* error) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) *error = "can't write " + path;
        return false;
    }
    auto writeNumber = [&file](uint32_t number) { file.write(reinterpret_cast<const char*>(&number), sizeof(number)); };
    auto writeText = [&](const std::string& text) {
        writeNumber(static_cast<uint32_t>(text.size()));
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    std::lock_guard<std::mutex> lock(_cacheMutex);
    size_t dimensions = 0;
    for (const auto& [text, vector] : _cache)
        if (!vector.empty()) dimensions = vector.size();
    file.write("VASDKEMB", 8);
    writeNumber(1);
    writeText(_encoder->name());
    writeNumber(static_cast<uint32_t>(dimensions));
    uint32_t count = 0;
    for (const auto& [text, vector] : _cache) count += vector.size() == dimensions && dimensions > 0 ? 1 : 0;
    writeNumber(count);
    for (const auto& [text, vector] : _cache) {
        if (vector.size() != dimensions || dimensions == 0) continue;
        writeText(text);
        file.write(reinterpret_cast<const char*>(vector.data()), static_cast<std::streamsize>(vector.size() * sizeof(float)));
    }
    if (!file) {
        if (error) *error = "can't write " + path;
        return false;
    }
    return true;
}

size_t EmbeddingNluProvider::loadCache(const std::string& path) const {
    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;
    auto readNumber = [&file](uint32_t& number) { return static_cast<bool>(file.read(reinterpret_cast<char*>(&number), sizeof(number))); };
    auto readText = [&](std::string& text) {
        uint32_t size = 0;
        if (!readNumber(size) || size > (1u << 20)) return false;
        text.resize(size);
        return static_cast<bool>(file.read(text.data(), size));
    };
    char magic[8] = {};
    uint32_t version = 0, dimensions = 0, count = 0;
    std::string encoder;
    if (!file.read(magic, 8) || std::string(magic, 8) != "VASDKEMB" || !readNumber(version) || version != 1 || !readText(encoder) ||
        !readNumber(dimensions) || !readNumber(count) || dimensions == 0 || dimensions > 8192) {
        spdlog::warn("[Embeddings] {} isn't an embedding cache", path);
        return 0;
    }
    if (encoder != _encoder->name()) {
        spdlog::info("[Embeddings] {} was made by {}, not {}: encoding again", path, encoder, _encoder->name());
        return 0;
    }
    std::map<std::string, std::vector<float>> read;
    for (uint32_t i = 0; i < count; ++i) {
        std::string text;
        std::vector<float> vector(dimensions);
        if (!readText(text) || !file.read(reinterpret_cast<char*>(vector.data()), static_cast<std::streamsize>(dimensions * sizeof(float)))) {
            spdlog::warn("[Embeddings] {} is cut short: {} of {} texts read", path, i, count);
            break;
        }
        read.emplace(std::move(text), std::move(vector));
    }
    std::lock_guard<std::mutex> lock(_cacheMutex);
    const size_t loaded = read.size();
    for (auto& entry : read) _cache.insert(std::move(entry));
    return loaded;
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
