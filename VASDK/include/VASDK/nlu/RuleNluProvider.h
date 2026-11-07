#pragma once

#include "Language.h"
#include "NluProvider.h"
#include "vasdk/flow/agent.pb.h"

#include <map>
#include <string>
#include <vector>

namespace va {
namespace nlu {

/**
 * @brief Intents from their examples, without a model: an intent matches when the words contain one
 *        of its examples, in order.
 *
 * - Words may be misspelled, as speech recognition does ("tyres", "apointment"): see wordMatch.
 * - An example may hold entities: "book an @service for @sys.date" matches "book an oil change for
 *   tuesday" (the words around them in order, the entities anywhere in the words).
 * - Negated, it doesn't match: "I don't want to book" isn't `book` (the languages' negations). Nor
 *   when a negative example is in the words.
 * - Built in, unless the agent declares its own of the name: sys.yes, sys.no, sys.cancel,
 *   sys.repeat, sys.help, sys.skip, with the examples of each of `languages`.
 *
 * score is the example's words matched (fuzzy ones count their similarity, each entity 1), so the
 * longest example wins; confidence is how fully it matched.
 */
class VASDK_EXPORT RuleNluProvider : public NluProvider {
public:
    RuleNluProvider(const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents, std::shared_ptr<const Languages> languages,
                    float minSimilarity = 0.8f);
    std::string name() const override { return "rules"; }
    NluResult understand(const NluRequest& request) const override;

    bool knows(const std::string& intent) const { return _intents.count(intent) > 0; }
    static bool isBuiltIn(const std::string& intent);
    /** The entities its examples name ("service", "sys.date"), for checking at load. */
    std::vector<std::string> entitiesUsed() const;

private:
    struct Example {
        std::vector<std::vector<std::string>> segments;   // the words between entities, in order
        std::vector<std::string> entities;
        size_t size = 0;                                  // words and entities
    };
    struct Intent {
        std::vector<Example> examples;
        std::vector<std::vector<std::string>> negatives;
    };
    void add(const std::string& name, const std::vector<std::string>& examples, const std::vector<std::string>& negatives);
    float score(const Example& example, const std::vector<std::string>& words, const NluRequest& request, bool negatable) const;

    std::map<std::string, Intent> _intents;
    std::vector<std::string> _order;   // declaration order: it breaks ties
    std::shared_ptr<const Languages> _languages;
    float _minSimilarity;
};

} // namespace nlu
} // namespace va
