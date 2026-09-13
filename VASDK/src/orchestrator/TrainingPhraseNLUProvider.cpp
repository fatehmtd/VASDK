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
