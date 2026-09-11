#pragma once

#include "vasdk_export.h"
#include <orchestrator/training.pb.h>

#include <google/protobuf/struct.pb.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace orchestrator {

// Milestone 1.3 (docs/reference/requestflow-roadmap.md §3) - a standalone
// compiler/matcher for spec §22.1's phrase grammar (Draft v2.9: {{name:type}}
// capture, (a|b|c) alternation, */. wildcards, uniform postfix ?, bare
// {canonical}, leading ! negative examples). No engine, no NLUProvider
// interface, no CEL dependency - see requestflow-roadmap.md's M1.3 entry for
// why this is deliberately self-contained. Wrapped behind the NLUProvider
// interface as TrainingPhraseNLUProvider in Phase 2 (M2.1).
class VASDK_EXPORT PhraseGrammarMatcher {
public:
    // Compiles every phrase in every intent, plus the entity dictionary.
    // Throws std::runtime_error - with the offending intent_name/phrase and a
    // specific reason - on: unbalanced parens/braces, a capture whose type
    // resolves neither to a built-in Parameter type keyword nor a declared
    // EntityGroup name, a capture name reused within one phrase (including
    // across alternation branches), an unresolved {canonical} reference, or a
    // duplicate EntityValue.canonical anywhere in the dictionary. Spec §22.1's
    // "fail loudly, not at match time" rule.
    explicit PhraseGrammarMatcher(const TrainingSet& trainingSet);

    struct Match {
        std::string intent_name;
        std::map<std::string, google::protobuf::Value> extracted_parameters;
        float confidence = 0.0f;   // specificity score - see spec §22.1; exact formula is an open decision
    };

    // Matches the whole utterance (not a substring) against every compiled
    // intent. An intent is a match iff at least one of its non-negative
    // phrases matches AND none of its leading-"!" negative phrases match.
    // Returned in descending confidence order.
    std::vector<Match> match(const std::string& utterance) const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

} // namespace orchestrator
} // namespace va
