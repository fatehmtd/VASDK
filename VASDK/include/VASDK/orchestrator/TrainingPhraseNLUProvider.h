#pragma once

#include "vasdk_export.h"
#include <VASDK/orchestrator/NLUProvider.h>
#include <VASDK/orchestrator/PhraseGrammarMatcher.h>
#include <orchestrator/training.pb.h>

#include <string>
#include <vector>

namespace va {
namespace orchestrator {

// Milestone 2.1 (docs/reference/requestflow-roadmap.md §4) - the concrete,
// fully local NLUProvider promised by spec §5/§22.2: wraps M1.3's
// PhraseGrammarMatcher behind the polymorphic interface, no model, no network
// call. The TrainingSet is compiled once at construction (matcher construction
// throws on malformed input - spec §22.1's "fail loudly, not at match time"
// rule), so initialize()/release() only track lifecycle state; nothing about
// this provider needs config supplied through initialize()'s generic Struct.
//
// NLUContext.interrupt_intent_names (§6.2) is now consulted - see detect()'s
// own comment below. NLUResult.co_intents (§6.1) is also populated, for a
// plain classification call only (context.expected_parameter_name empty):
// spec §5 suggests "running its phrase matcher against the remainder of the
// utterance after the top match's span is consumed (splitting on
// conjunctions is the common case)" - since PhraseGrammarMatcher only ever
// matches a whole string (no span/substring concept - see its own header),
// this implements that literally by splitting the utterance into clauses (on
// " and ", ", and ", "," and a couple of common variants -
// splitOnConjunctions() in the .cpp) and matching each clause independently
// as its own tiny "utterance", rather than teaching the matcher itself about
// substrings. Because of that same "whole string only" constraint, a
// genuinely multi-intent utterance essentially never matches as a whole in
// the first place - so when it doesn't, detect() also uses the clause split
// to *find* top_intent itself (the best match across all clauses), not just
// co_intents; an utterance that DOES match as a whole (the ordinary,
// single-intent case) is unaffected, since that whole-utterance winner is
// always preferred first. A clause's own best match becomes a co_intent if
// it differs from top_intent, isn't already an interrupt intent, and isn't a
// duplicate of an earlier clause's pick. This is a heuristic, not a real
// span/parse: an utterance with no recognized conjunction never gets split,
// and co_intents stays empty - exactly Draft v2.1's behavior.
// capture_defaults (spec §26) are a Phase 4 concern applied by the engine, not
// this provider - detect() returns exactly what the phrase matched, nothing
// filled in for absent optional captures.
class VASDK_EXPORT TrainingPhraseNLUProvider : public NLUProvider {
public:
    // Compiles trainingSet immediately - throws std::runtime_error on any of
    // PhraseGrammarMatcher's load-time validation failures (see its header).
    explicit TrainingPhraseNLUProvider(TrainingSet trainingSet);

    bool initialize(const google::protobuf::Struct& config) override;
    void release() override;

    // Matches the utterance against the compiled TrainingSet. Spec §6.2 -
    // context.interrupt_intent_names is checked first, regardless of what
    // else this call is for: if any match's intent_name is in that set, the
    // highest-confidence one among them wins as top_intent outright (skipping
    // both the candidate-set restriction and the expected_parameter_name
    // preference below), since an interrupt is meant to preempt whatever step
    // 2 was otherwise doing. Otherwise: if context.expected_parameter_name is
    // set, prefers the highest-confidence match that actually captured that
    // name (falling back to the overall best match if none did); otherwise
    // just returns the best match as top_intent, plus (see the class comment)
    // any additional distinct intents found by splitting the utterance on
    // conjunctions, in co_intents. All candidate matches - restricted to
    // setCandidateIntents()'s set, if one was given - are returned in
    // ranked_intents, descending confidence, exactly as
    // PhraseGrammarMatcher::match() produced them.
    va::orchestrator::NLUResult detect(const std::string& utterance,
                                        const va::orchestrator::NLUContext& context) override;

    // Restricts detect()'s candidate matches to this set of intent names
    // (e.g. the active node's own intent routes). An empty vector (the
    // default, and the effect of never calling this) means unrestricted.
    void setCandidateIntents(const std::vector<std::string>& names) override;

private:
    TrainingSet _trainingSet;
    PhraseGrammarMatcher _matcher;
    std::vector<std::string> _candidateIntents;
    bool _initialized = false;
};

} // namespace orchestrator
} // namespace va
