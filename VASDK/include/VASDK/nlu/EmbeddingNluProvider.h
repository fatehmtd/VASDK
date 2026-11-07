#pragma once

#include "NluProvider.h"
#include "vasdk/flow/agent.pb.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va {
namespace nlu {

/**
 * @brief Turns a sentence into a vector whose direction is its meaning: sentences that mean the
 *        same point the same way. A small on-device model (MiniLM in VASDK_3P_IMPL) implements it.
 */
class VASDK_EXPORT SentenceEncoder {
public:
    virtual ~SentenceEncoder() = default;
    virtual std::string name() const = 0;
    /** The sentence's vector, normalized to length 1; empty when it can't be encoded. Thread-safe. */
    virtual std::vector<float> encode(const std::string& sentence) const = 0;
    /** A vector per word of `words`, in its sentence (the tagger's features); empty when the
     *  encoder can't give them. Thread-safe. */
    virtual std::vector<std::vector<float>> encodeWords(const std::vector<std::string>& words) const {
        (void)words;
        return {};
    }
};

/** How alike two normalized vectors are: their cosine, -1 to 1. */
VASDK_EXPORT float cosine(const std::vector<float>& a, const std::vector<float>& b);

/**
 * @brief Intents by meaning: the words are compared with every example of every candidate
 *        intent, and an intent scores as its closest example. Paraphrases the rules miss ("is the
 *        shop open saturday" for hours) are found. Examples are encoded once and kept. Thread-safe.
 *
 * Use it after the rules in a CascadeNluProvider: `{rules, embeddings}`, so a model only answers
 * what the examples don't. score and confidence are the cosine (0 below `minSimilarity`).
 */
class VASDK_EXPORT EmbeddingNluProvider : public NluProvider {
public:
    EmbeddingNluProvider(std::shared_ptr<const SentenceEncoder> encoder,
                         const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents, float minSimilarity = 0.3f);
    std::string name() const override { return _encoder->name(); }
    NluResult understand(const NluRequest& request) const override;
    /** The closest of `texts` to `sentence`: its index and cosine; -1 when none reaches `minSimilarity`. */
    std::pair<int, float> closest(const std::string& sentence, const std::vector<std::string>& texts, float minSimilarity) const;

    /** Encodes every example now, and `more` (an agent's knowledge questions): what isn't encoded yet. How many were. */
    size_t precompute(const std::vector<std::string>& more = {}) const;
    /**
     * Saves what's encoded to `path`, for this encoder (its name and vector size), so a later start
     * reads it instead of encoding again (an app's first start on a phone). False, with `error`.
     */
    bool saveCache(const std::string& path, std::string* error = nullptr) const;
    /** Reads what saveCache wrote with the same encoder: how many texts. 0 for a missing file or another encoder's. */
    size_t loadCache(const std::string& path) const;

private:
    const std::vector<float>& encoded(const std::string& text) const;

    std::shared_ptr<const SentenceEncoder> _encoder;
    std::vector<std::pair<std::string, std::vector<std::string>>> _intents;   // name, examples (entities as their names)
    float _minSimilarity;
    mutable std::mutex _cacheMutex;
    mutable std::map<std::string, std::vector<float>> _cache;
};

} // namespace nlu
} // namespace va
