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
