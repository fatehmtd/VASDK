#include <VASDK/orchestrator/LLMNLUProvider.h>

#include <VASDK/Frame.h>
#include <algorithm>
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

    ss << "\nThe utterance may independently express more than one of the intents above at once "
          "(e.g. \"play some jazz and turn up the AC\" naming two unrelated requests) - list any "
          "such additional intents, besides your single top pick, in \"co_intents\". Leave it "
          "empty if the utterance only expresses one request.\n"
          "\nRespond with ONLY a single JSON object, no other text, in exactly this shape:\n"
          "{\"intent\": \"<one of the intent names above, or empty string if none apply>\", "
          "\"confidence\": <number 0.0-1.0>, "
          "\"parameters\": {<captured values, keyed by the {{name:type}} names shown above>}, "
          "\"alternatives\": [{\"name\": \"<intent name>\", \"confidence\": <number>}, ...], "
          "\"co_intents\": [{\"name\": \"<another intent name>\", \"confidence\": <number>}, ...]}\n";

    return ss.str();
}

NLUResult LLMNLUProvider::detect(const std::string& utterance, const NLUContext& context) {
    NLUResult result;

    if (!_transport) {
        spdlog::error("[LLMNLUProvider] detect() called with a null transport");
        return result;
    }

    // Milestone 2.5 (spec §9.5/§6 step 2) - slot-filling for a static-form or
    // `expect` parameter is a differently-shaped call from intent
    // classification; see the class comment on why it can't reuse
    // buildClassificationPrompt's "parameters" field.
    if (!context.expected_parameter_name().empty()) {
        return detectParameterExtraction(utterance, context);
    }

    std::vector<std::string> candidates(context.candidate_intent_names().begin(),
                                         context.candidate_intent_names().end());
    if (candidates.empty()) candidates = _candidateIntents;

    // Spec §6.2 - interrupt names are always eligible for classification too,
    // even on a node whose own routes wouldn't otherwise offer them (that's
    // exactly the generative-node interrupt-prelude call's shape: empty
    // ordinary candidates, interrupt names only). The model just sees them as
    // more entries in "available intents" - the engine, not this prompt,
    // treats a matched interrupt specially once detect() returns.
    for (const auto& name : context.interrupt_intent_names()) {
        if (std::find(candidates.begin(), candidates.end(), name) == candidates.end()) {
            candidates.push_back(name);
        }
    }

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
    // Spec §6.1 - co_intents, this provider's own top_intent excluded (it's
    // "additional intents beyond top_intent" per spec §5) in case the model
    // repeats its own pick despite the prompt asking for "additional" ones.
    if (parsed.contains("co_intents") && parsed["co_intents"].is_array()) {
        for (const auto& co : parsed["co_intents"]) {
            if (!co.is_object() || !co.contains("name") || !co["name"].is_string()) continue;
            std::string name = co["name"].get<std::string>();
            if (name.empty() || name == result.top_intent()) continue;
            auto* coIntent = result.add_co_intents();
            coIntent->set_name(name);
            if (co.contains("confidence") && co["confidence"].is_number()) {
                coIntent->set_confidence(co["confidence"].get<float>());
            }
        }
    }

    return result;
}

std::string LLMNLUProvider::buildExtractionPrompt(const std::string& parameterName,
                                                    const std::vector<std::string>& interruptIntentNames) const {
    std::ostringstream ss;
    ss << "You are a slot-filling engine for a voice assistant. The user was just asked to "
          "provide a value for \"" << parameterName << "\". Given their reply, extract that "
          "value if it's present - do not guess or invent one if it isn't.\n";

    // Spec §6.2 - the extraction prompt has no other notion of intents, so
    // interrupt checking needs its own section here rather than reusing
    // buildClassificationPrompt's candidate list the way the plain
    // classification call does (see detect()). An interrupt match takes
    // priority over extraction - "no parameter is extracted or stored" when
    // one applies.
    if (!interruptIntentNames.empty()) {
        ss << "\nBefore extracting, check whether the reply instead expresses one of these "
              "unrelated requests, which take priority over answering the question:\n";
        for (const auto& name : interruptIntentNames) {
            ss << "- " << name << "\n";
            const IntentTrainingData* data = findTrainingData(name);
            if (!data) continue;
            for (const auto& phrase : data->phrases()) {
                if (!phrase.empty() && phrase.front() == '!') continue;
                ss << "  Example: \"" << phrase << "\"\n";
            }
        }
        ss << "If one of these applies, respond with ONLY {\"interrupt_intent\": \"<name>\"} "
              "and nothing else.\n";
    }

    ss << "\nOtherwise respond with ONLY a single JSON object, no other text, in exactly this shape:\n"
          "{\"value\": <the extracted value as a string, number, or boolean, or null if the "
          "reply doesn't provide one>}\n";
    return ss.str();
}

NLUResult LLMNLUProvider::detectParameterExtraction(const std::string& utterance, const NLUContext& context) {
    NLUResult result;
    const std::string& parameterName = context.expected_parameter_name();
    std::vector<std::string> interruptNames(context.interrupt_intent_names().begin(),
                                             context.interrupt_intent_names().end());

    _transport->clearHistory();
    _transport->addMessageToHistory(va::llm::MessageRole::SYSTEM,
                                     buildExtractionPrompt(parameterName, interruptNames));

    auto frame = std::make_shared<transport::TextFrame>();
    frame->text = utterance;
    frame->timestamp = transport::Frame::getCurrentTimestamp();

    if (!_transport->sendFrame(frame)) {
        spdlog::warn("[LLMNLUProvider] sendFrame() failed for slot extraction utterance \"{}\"", utterance);
        return result;
    }

    const auto& history = _transport->getHistory();
    if (history.empty() || history.back().role != va::llm::MessageRole::ASSISTANT) {
        spdlog::warn("[LLMNLUProvider] no assistant response for slot extraction utterance \"{}\"", utterance);
        return result;
    }

    const std::string& raw = history.back().content;
    json parsed;
    try {
        parsed = json::parse(raw);
    } catch (const std::exception& e) {
        spdlog::warn("[LLMNLUProvider] failed to parse extraction response as JSON: {} (raw: \"{}\")",
                     e.what(), raw);
        return result;
    }

    if (parsed.is_object() && parsed.contains("interrupt_intent") && parsed["interrupt_intent"].is_string()) {
        result.set_top_intent(parsed["interrupt_intent"].get<std::string>());
        result.set_confidence(1.0f);
        return result;
    }

    if (parsed.is_object() && parsed.contains("value") && !parsed["value"].is_null()) {
        jsonToProtoValue(parsed["value"], (*result.mutable_extracted_parameters())[parameterName]);
    }
    return result;
}

} // namespace orchestrator
} // namespace va
