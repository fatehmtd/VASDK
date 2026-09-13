#include <VASDK/orchestrator/LLMNLUProvider.h>

#include <VASDK/Frame.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sstream>

using json = nlohmann::json;

namespace va {
namespace orchestrator {

namespace {

void jsonToProtoValue(const json& j, google::protobuf::Value& out) {
    if (j.is_string()) {
        out.set_string_value(j.get<std::string>());
    } else if (j.is_number()) {
        out.set_number_value(j.get<double>());
    } else if (j.is_boolean()) {
        out.set_bool_value(j.get<bool>());
    } else if (j.is_object()) {
        auto* s = out.mutable_struct_value();
        for (const auto& [key, value] : j.items()) {
            jsonToProtoValue(value, (*s->mutable_fields())[key]);
        }
    } else if (j.is_array()) {
        auto* l = out.mutable_list_value();
        for (const auto& value : j) {
            jsonToProtoValue(value, *l->add_values());
        }
    } else {
        out.set_null_value(google::protobuf::NULL_VALUE);
    }
}

} // namespace

LLMNLUProvider::LLMNLUProvider(va::llm::LLMTransport* transport, TrainingSet trainingSet)
    : _transport(transport)
    , _trainingSet(std::move(trainingSet)) {}

bool LLMNLUProvider::initialize(const google::protobuf::Struct& /*config*/) {
    if (!_transport) {
        spdlog::error("[LLMNLUProvider] initialize() called with a null transport");
        return false;
    }
    _initialized = true;
    return true;
}

void LLMNLUProvider::release() {
    _initialized = false;
}

void LLMNLUProvider::setCandidateIntents(const std::vector<std::string>& names) {
    _candidateIntents = names;
}

const IntentTrainingData* LLMNLUProvider::findTrainingData(const std::string& intentName) const {
    for (const auto& data : _trainingSet.intents()) {
        if (data.intent_name() == intentName) return &data;
    }
    return nullptr;
}

std::string LLMNLUProvider::buildClassificationPrompt(const std::vector<std::string>& candidateNames) const {
    std::ostringstream ss;
    ss << "You are an intent-classification engine for a voice assistant. Given a single "
          "user utterance, decide which one of the available intents (if any) it expresses, "
          "and extract any parameters shown in that intent's examples as {{name:type}}.\n\n"
          "Available intents:\n";

    // An explicit candidate list restricts to just those names (falling back to a bare
    // name with no examples if the TrainingSet has none for it - spec §22.3's own
    // "zero examples, still classifiable" fallback); an empty list means "every intent
    // this TrainingSet knows about".
    std::vector<std::string> names = candidateNames;
    if (names.empty()) {
        for (const auto& data : _trainingSet.intents()) names.push_back(data.intent_name());
    }

    for (const auto& name : names) {
        ss << "- " << name << "\n";
        const IntentTrainingData* data = findTrainingData(name);
        if (!data) continue;
        for (const auto& phrase : data->phrases()) {
            if (!phrase.empty() && phrase.front() == '!') {
                ss << "  NOT an example of this intent: \"" << phrase.substr(1) << "\"\n";
            } else {
                ss << "  Example: \"" << phrase << "\"\n";
            }
        }
    }

    ss << "\nRespond with ONLY a single JSON object, no other text, in exactly this shape:\n"
          "{\"intent\": \"<one of the intent names above, or empty string if none apply>\", "
          "\"confidence\": <number 0.0-1.0>, "
          "\"parameters\": {<captured values, keyed by the {{name:type}} names shown above>}, "
          "\"alternatives\": [{\"name\": \"<intent name>\", \"confidence\": <number>}, ...]}\n";

    return ss.str();
}

NLUResult LLMNLUProvider::detect(const std::string& utterance, const NLUContext& context) {
    NLUResult result;

    if (!_transport) {
        spdlog::error("[LLMNLUProvider] detect() called with a null transport");
        return result;
    }

    std::vector<std::string> candidates(context.candidate_intent_names().begin(),
                                         context.candidate_intent_names().end());
    if (candidates.empty()) candidates = _candidateIntents;

    _transport->clearHistory();
    _transport->addMessageToHistory(va::llm::MessageRole::SYSTEM, buildClassificationPrompt(candidates));

    auto frame = std::make_shared<transport::TextFrame>();
    frame->text = utterance;
    frame->timestamp = transport::Frame::getCurrentTimestamp();

    if (!_transport->sendFrame(frame)) {
        spdlog::warn("[LLMNLUProvider] sendFrame() failed for utterance \"{}\"", utterance);
        return result;
    }

    const auto& history = _transport->getHistory();
    if (history.empty() || history.back().role != va::llm::MessageRole::ASSISTANT) {
        spdlog::warn("[LLMNLUProvider] no assistant response for utterance \"{}\"", utterance);
        return result;
    }

    const std::string& raw = history.back().content;
    json parsed;
    try {
        parsed = json::parse(raw);
    } catch (const std::exception& e) {
        spdlog::warn("[LLMNLUProvider] failed to parse classification response as JSON: {} (raw: \"{}\")",
                     e.what(), raw);
        return result;
    }
    if (!parsed.is_object()) {
        spdlog::warn("[LLMNLUProvider] classification response is not a JSON object (raw: \"{}\")", raw);
        return result;
    }

    if (parsed.contains("intent") && parsed["intent"].is_string()) {
        result.set_top_intent(parsed["intent"].get<std::string>());
    }
    if (parsed.contains("confidence") && parsed["confidence"].is_number()) {
        result.set_confidence(parsed["confidence"].get<float>());
    }
    if (parsed.contains("parameters") && parsed["parameters"].is_object()) {
        for (const auto& [name, value] : parsed["parameters"].items()) {
            jsonToProtoValue(value, (*result.mutable_extracted_parameters())[name]);
        }
    }
    if (parsed.contains("alternatives") && parsed["alternatives"].is_array()) {
        for (const auto& alt : parsed["alternatives"]) {
            if (!alt.is_object()) continue;
            auto* ranked = result.add_ranked_intents();
            if (alt.contains("name") && alt["name"].is_string()) {
                ranked->set_name(alt["name"].get<std::string>());
            }
            if (alt.contains("confidence") && alt["confidence"].is_number()) {
                ranked->set_confidence(alt["confidence"].get<float>());
            }
        }
    }

    return result;
}

} // namespace orchestrator
} // namespace va
