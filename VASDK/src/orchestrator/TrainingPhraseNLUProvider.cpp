#include <VASDK/orchestrator/TrainingPhraseNLUProvider.h>

#include <algorithm>

namespace va {
namespace orchestrator {

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

    if (!_candidateIntents.empty()) {
        matches.erase(std::remove_if(matches.begin(), matches.end(),
                          [this](const PhraseGrammarMatcher::Match& m) {
                              return std::find(_candidateIntents.begin(), _candidateIntents.end(),
                                                m.intent_name) == _candidateIntents.end();
                          }),
                      matches.end());
    }

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

    if (winner) {
        result.set_top_intent(winner->intent_name);
        result.set_confidence(winner->confidence);
        for (const auto& [name, value] : winner->extracted_parameters) {
            (*result.mutable_extracted_parameters())[name] = value;
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
