#pragma once

#include "vasdk_export.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace nlu {

/** An intent the words may mean. */
struct IntentMatch {
    std::string intent;
    float score = 0;        // how much of the words it explains; rules: the example's words matched (fuzzy ones count less)
    float confidence = 0;   // 0 to 1: how sure the stage is
};

/** What to understand. */
struct NluRequest {
    std::string text;
    std::vector<std::string> candidates;   // the intents that may be meant; empty: any
    // Whether the words name a value of an entity ("service", "sys.date"), for @entity in examples.
    std::function<bool(const std::string& entity)> mentions;
};

/** What the words mean: candidate intents, best first. */
struct NluResult {
    std::vector<IntentMatch> intents;
    std::string stage;      // the provider that decided: "rules", "minilm", "llm"
};

/**
 * @brief Understands what the user means. Rules (examples and keywords) come built in; a small
 *        on-device model and an LLM add further stages (CascadeNluProvider). Thread-safe.
 */
class VASDK_EXPORT NluProvider {
public:
    virtual ~NluProvider() = default;
    virtual std::string name() const = 0;
    virtual NluResult understand(const NluRequest& request) const = 0;
};

/**
 * @brief Stages tried in order until one is sure enough: rules first, then a model, then an LLM.
 *        None sure: the most confident answer among them.
 */
class VASDK_EXPORT CascadeNluProvider : public NluProvider {
public:
    CascadeNluProvider(std::vector<std::shared_ptr<const NluProvider>> stages, float minConfidence = 0.55f)
        : _stages(std::move(stages)), _minConfidence(minConfidence) {}
    std::string name() const override { return "cascade"; }
    NluResult understand(const NluRequest& request) const override;

private:
    std::vector<std::shared_ptr<const NluProvider>> _stages;
    float _minConfidence;
};

/** How alike two words are, 0 to 1: 1 minus their edit distance (a swap counts once) over the longer one's length. */
VASDK_EXPORT float wordSimilarity(const std::string& a, const std::string& b);
/**
 * How well `heard` stands for `expected`: 1 when the same; their similarity when both have 4 or
 * more letters and it's at least `minSimilarity` (an STT misspelling: "tyres", "apointment"); else 0.
 */
VASDK_EXPORT float wordMatch(const std::string& heard, const std::string& expected, float minSimilarity);
/** Where `phrase` occurs in `words`, word for word by wordMatch: each start, and the sum of its words' matches. */
VASDK_EXPORT std::vector<std::pair<size_t, float>> findPhrase(const std::vector<std::string>& words,
                                                              const std::vector<std::string>& phrase, float minSimilarity);

} // namespace nlu
} // namespace va
