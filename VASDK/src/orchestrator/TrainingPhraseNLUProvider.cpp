#include <VASDK/orchestrator/TrainingPhraseNLUProvider.h>

#include <algorithm>
#include <cctype>

namespace va {
namespace orchestrator {

namespace {

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Spec §5 - "splitting on conjunctions is the common case" for finding more
// than one independently-actionable clause in an utterance without teaching
// PhraseGrammarMatcher itself a span/substring concept it doesn't have (see
// its own header - match() only ever matches a whole string). Delimiters are
// matched case-insensitively but sliced from the original string, so
// original casing survives into each clause. Deliberately simple: this is a
// heuristic an author's own phrasing/training data may need to account for
// (e.g. an entity value that legitimately contains "and"), not a real parse.
std::vector<std::string> splitOnConjunctions(const std::string& utterance) {
    static const std::vector<std::string> kDelimiters = {
        " and then ", " and also ", " and ", ", and ", ","};
    std::vector<std::string> segments{utterance};
    for (const auto& delim : kDelimiters) {
        std::vector<std::string> next;
        for (const auto& segment : segments) {
            std::string lower = toLowerAscii(segment);
            size_t start = 0;
            while (true) {
                size_t pos = lower.find(delim, start);
                if (pos == std::string::npos) {
                    next.push_back(segment.substr(start));
                    break;
                }
                next.push_back(segment.substr(start, pos - start));
                start = pos + delim.size();
            }
        }
        segments = std::move(next);
    }
    std::vector<std::string> trimmed;
    for (const auto& s : segments) {
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = s.find_last_not_of(" \t");
        trimmed.push_back(s.substr(b, e - b + 1));
    }
    return trimmed;
}

} // namespace

TrainingPhraseNLUProvider::TrainingPhraseNLUProvider(TrainingSet trainingSet)
    : _trainingSet(std::move(trainingSet))
    , _matcher(_trainingSet) {}

bool TrainingPhraseNLUProvider::initialize(const google::protobuf::Struct& /*config*/) {
    _initialized = true;
    return true;
}

void TrainingPhraseNLUProvider::release() {
    _initialized = false;
}

void TrainingPhraseNLUProvider::setCandidateIntents(const std::vector<std::string>& names) {
    _candidateIntents = names;
}

NLUResult TrainingPhraseNLUProvider::detect(const std::string& utterance, const NLUContext& context) {
    auto matches = _matcher.match(utterance);

    // Spec §6.2 - checked first, regardless of what else this call is for
    // (an expected_parameter_name extraction or an ordinary classification):
    // an interrupt match always wins, before the candidate-set restriction or
    // the expected_parameter_name preference below even run.
    if (!context.interrupt_intent_names().empty()) {
        const PhraseGrammarMatcher::Match* interruptWinner = nullptr;
        for (const auto& m : matches) {
            bool isInterrupt = std::find(context.interrupt_intent_names().begin(),
                                          context.interrupt_intent_names().end(),
                                          m.intent_name) != context.interrupt_intent_names().end();
            if (!isInterrupt) continue;
            if (!interruptWinner || m.confidence > interruptWinner->confidence) interruptWinner = &m;
        }
        if (interruptWinner) {
            NLUResult result;
            result.set_top_intent(interruptWinner->intent_name);
            result.set_confidence(interruptWinner->confidence);
            for (const auto& [name, value] : interruptWinner->extracted_parameters) {
                (*result.mutable_extracted_parameters())[name] = value;
            }
            for (const auto& m : matches) {
                auto* ranked = result.add_ranked_intents();
                ranked->set_name(m.intent_name);
                ranked->set_confidence(m.confidence);
            }
            return result;
        }
    }

    auto filterByCandidates = [this](std::vector<PhraseGrammarMatcher::Match>& v) {
        if (_candidateIntents.empty()) return;
        v.erase(std::remove_if(v.begin(), v.end(),
                                [this](const PhraseGrammarMatcher::Match& m) {
                                    return std::find(_candidateIntents.begin(), _candidateIntents.end(),
                                                      m.intent_name) == _candidateIntents.end();
                                }),
                v.end());
    };

    filterByCandidates(matches);

    NLUResult result;

    const PhraseGrammarMatcher::Match* winner = nullptr;
    if (!context.expected_parameter_name().empty()) {
        for (const auto& m : matches) {
            if (m.extracted_parameters.count(context.expected_parameter_name())) {
                winner = &m;
                break;
            }
        }
    }
    if (!winner && !matches.empty()) {
        winner = &matches.front();
    }

    // Spec §5/§6.1 - PhraseGrammarMatcher only ever matches a whole string
    // (never a substring - see its own header), so a genuinely multi-intent
    // utterance ("play some jazz and turn up the ac") essentially never
    // matches any single trained phrase in full, and `winner` above stays
    // null for exactly the utterances this feature exists for. When that
    // happens, on a plain classification call, fall back to matching each
    // conjunction-split clause independently and taking the best match
    // across all of them as top_intent - other distinct-intent clause
    // matches become co_intents below, from this same pass. An utterance
    // that DOES match as a whole (the ordinary, common case) never reaches
    // this branch, so existing single-intent behavior is unchanged.
    std::vector<PhraseGrammarMatcher::Match> clauseFallbackMatches;
    bool usedClauseFallback = false;
    if (!winner && context.expected_parameter_name().empty()) {
        auto clauses = splitOnConjunctions(utterance);
        if (clauses.size() > 1) {
            usedClauseFallback = true;
            for (const auto& clause : clauses) {
                auto clauseMatches = _matcher.match(clause);
                filterByCandidates(clauseMatches);
                if (!clauseMatches.empty()) clauseFallbackMatches.push_back(clauseMatches.front());
            }
            std::stable_sort(clauseFallbackMatches.begin(), clauseFallbackMatches.end(),
                              [](const PhraseGrammarMatcher::Match& a, const PhraseGrammarMatcher::Match& b) {
                                  return a.confidence > b.confidence;
                              });
            if (!clauseFallbackMatches.empty()) winner = &clauseFallbackMatches.front();
        }
    }

    if (winner) {
        result.set_top_intent(winner->intent_name);
        result.set_confidence(winner->confidence);
        for (const auto& [name, value] : winner->extracted_parameters) {
            (*result.mutable_extracted_parameters())[name] = value;
        }

        // Spec §5/§6.1 - co_intents, plain classification calls only ("when
        // the call is a plain intent-classification call (no
        // expected_parameter_name)"). Reuses clauseFallbackMatches when the
        // fallback above already ran (no need to re-split/re-match); a
        // whole-utterance winner instead gets its own fresh clause pass, in
        // case the utterance still contains a conjunction (e.g. a wildcard
        // phrase happened to also match the combined string as a whole).
        if (context.expected_parameter_name().empty()) {
            std::vector<std::string> seen{winner->intent_name};
            auto considerCoIntent = [&](const PhraseGrammarMatcher::Match& m) {
                if (std::find(seen.begin(), seen.end(), m.intent_name) != seen.end()) return;
                bool isInterrupt = std::find(context.interrupt_intent_names().begin(),
                                              context.interrupt_intent_names().end(),
                                              m.intent_name) != context.interrupt_intent_names().end();
                if (isInterrupt) return;
                seen.push_back(m.intent_name);
                auto* co = result.add_co_intents();
                co->set_name(m.intent_name);
                co->set_confidence(m.confidence);
            };
            if (usedClauseFallback) {
                for (const auto& m : clauseFallbackMatches) considerCoIntent(m);
            } else {
                auto clauses = splitOnConjunctions(utterance);
                if (clauses.size() > 1) {
                    for (const auto& clause : clauses) {
                        auto clauseMatches = _matcher.match(clause);
                        filterByCandidates(clauseMatches);
                        if (!clauseMatches.empty()) considerCoIntent(clauseMatches.front());
                    }
                }
            }
        }
    }

    for (const auto& m : matches) {
        auto* ranked = result.add_ranked_intents();
        ranked->set_name(m.intent_name);
        ranked->set_confidence(m.confidence);
    }

    return result;
}

} // namespace orchestrator
} // namespace va
