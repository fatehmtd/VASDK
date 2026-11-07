#pragma once

#include "EntityExtractor.h"
#include "EmbeddingNluProvider.h"

#include <memory>

namespace va {
namespace nlu {

/**
 * @brief Entities found by a tagger trained on the agent's own sentences, on the sentence
 *        encoder's word vectors (SentenceEncoder::encodeWords; MiniLM gives them). Training runs
 *        once, when it's made: each intent example with its "@entity" placeholders filled in
 *        turn with that entity's examples, each entity example alone, and the examples without a
 *        placeholder as words that aren't one. More examples with placeholders, better tagging.
 *        Each word is tagged from its vector and its neighbours'. Thread-safe once made.
 */
class VASDK_EXPORT TaggerEntityExtractor : public EntityExtractor {
public:
    TaggerEntityExtractor(std::shared_ptr<const SentenceEncoder> encoder, const std::vector<EntityLabel>& labels,
                          const std::vector<std::string>& examples);
    std::string name() const override { return "tagger"; }
    /** False when the encoder gives no word vectors, or there was nothing to learn: it finds nothing. */
    bool trained() const { return !_weights.empty(); }
    /** The sentences it learned from. */
    size_t sentences() const { return _sentences; }
    std::vector<EntitySpan> extract(const std::vector<std::string>& words, const std::vector<EntityLabel>& labels,
                                    bool sensitive) const override;

private:
    std::vector<float> features(const std::vector<std::vector<float>>& vectors, size_t word) const;
    std::vector<float> probabilities(const std::vector<float>& features) const;

    std::shared_ptr<const SentenceEncoder> _encoder;
    std::vector<std::string> _classes;   // "" (not an entity), then the entities
    size_t _dimension = 0;               // a word vector's size
    std::vector<float> _weights;         // classes × (3 × _dimension + 1)
    size_t _sentences = 0;
};

} // namespace nlu
} // namespace va
