#pragma once

#include <VASDK/vasdk_export.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace va {
namespace nlu {

/** An intent the words may mean. */
struct IntentMatch {
    std::string intent;
    float score = 0;        // how much of the words it explains; rules: the example's words matched (fuzzy ones count less)
    float confidence = 0;   // 0 to 1: how sure the stage is
    float explains = 1;     // 0 to 1: the share of what was said it accounts for; rules: 1/21 for "hey" opening a 21-word question
};

/** What to understand. */
struct NluRequest {
    std::string text;
    std::vector<std::string> candidates;   // the intents that may be meant; empty: any
    // Whether the words name a value of an entity ("service", "sys.date"), for @entity in examples.
    std::function<bool(const std::string& entity)> mentions;
    // The words may hold a sensitive value (the answer to a sensitive slot): stages that send
    // them off the device (an LLM) don't look at them.
    bool sensitive = false;
};

/** What the words mean: candidate intents, best first. */
struct NluResult {
    std::vector<IntentMatch> intents;
    std::string stage;      // the provider that decided: "rules", "minilm", "llm"
    bool noneMeant = false; // the stage is sure the words mean none of the intents (an LLM's "none")
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
    /**
     * A stage's answer is sure at `minConfidence`, weighed by how much of the words it explains: a
     * match may stand for a sentence of up to `keywordWords` words a matched word ("let me talk to
     * a person" for "person"); in a longer one it counts for less, and the next stage is asked.
     */
    CascadeNluProvider(std::vector<std::shared_ptr<const NluProvider>> stages, float minConfidence = 0.55f, float keywordWords = 6.0f)
        : _stages(std::move(stages)), _minConfidence(minConfidence), _keywordWords(keywordWords) {}
    std::string name() const override { return "cascade"; }
    NluResult understand(const NluRequest& request) const override;

private:
    std::vector<std::shared_ptr<const NluProvider>> _stages;
    float _minConfidence;
    float _keywordWords;
};

/** How alike two words are, 0 to 1: 1 minus their edit distance (a swap counts once) over the longer one's length. */
VASDK_EXPORT float wordSimilarity(const std::string& a, const std::string& b);
/**
 * How well `heard` stands for `expected`: 1 when the same; their similarity when both are words of
 * 4 or more letters (no digits) and it's at least `minSimilarity` (an STT misspelling: "tyres",
 * "apointment"); else 0.
 */
VASDK_EXPORT float wordMatch(const std::string& heard, const std::string& expected, float minSimilarity);
/**
 * How a word sounds, by Double Metaphone, for names a speech recognizer spells its own way: a
 * primary code and an alternate, for spellings with two common sounds, up to 4 sounds each.
 * "Catherine" and "Katherine" → K0RN (alternate KTRN); "Schmidt" → XMT, SMT; "Smith" → SM0, XMT.
 */
VASDK_EXPORT std::pair<std::string, std::string> doubleMetaphone(const std::string& word);
/** doubleMetaphone's primary code. */
VASDK_EXPORT std::string phoneticKey(const std::string& word);
/** Two words that sound alike: one's primary or alternate code is one of the other's ("Smith", "Schmidt"). */
VASDK_EXPORT bool soundsAlike(const std::string& a, const std::string& b);
/** Where `phrase` occurs in `words`, word for word by wordMatch: each start, and the sum of its words' matches. */
VASDK_EXPORT std::vector<std::pair<size_t, float>> findPhrase(const std::vector<std::string>& words,
                                                              const std::vector<std::string>& phrase, float minSimilarity);

} // namespace nlu
} // namespace va
