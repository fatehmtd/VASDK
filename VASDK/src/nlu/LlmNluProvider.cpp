#include "nlu/LlmNluProvider.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace va {
namespace nlu {

LlmNluProvider::LlmNluProvider(std::shared_ptr<const llm::Completion> completion,
                               const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents)
    : _completion(std::move(completion)) {
    for (const auto& intent : intents)
        _intents.emplace_back(intent.name(), std::vector<std::string>(intent.examples().begin(), intent.examples().end()));
}

NluResult LlmNluProvider::understand(const NluRequest& request) const {
    NluResult result;
    result.stage = name();
    if (!_completion || request.sensitive || request.text.find_first_not_of(" \t\n") == std::string::npos) return result;

    std::vector<std::string> names;
    std::string listing;
    for (const auto& [intent, examples] : _intents) {
        if (!request.candidates.empty() &&
            std::find(request.candidates.begin(), request.candidates.end(), intent) == request.candidates.end())
            continue;
        names.push_back(intent);
        listing += "- " + intent;
        for (size_t i = 0; i < examples.size() && i < 6; ++i) listing += (i ? "; " : ": ") + examples[i];
        listing += "\n";
    }
    if (names.empty()) return result;

    const std::string instructions =
        "You find what a user of a voice assistant means. The intents, with example phrasings "
        "(@name stands for a value of that kind):\n" + listing +
        "Reply with only a JSON object: {\"intent\": \"<one of the intent names, or none>\", "
        "\"confidence\": <0 to 1>}. Say none when the words mean none of them.";
    const auto reply = _completion->complete({{llm::MessageRole::SYSTEM, instructions, 0},
                                              {llm::MessageRole::USER, request.text, 0}});
    if (!reply) return result;
    try {
        const auto answer = nlohmann::json::parse(llm::jsonObjectIn(*reply));
        const auto intent = answer.value("intent", std::string());
        const float confidence = std::clamp(answer.value("confidence", 0.0f), 0.0f, 1.0f);
        if (std::find(names.begin(), names.end(), intent) != names.end() && confidence > 0)
            result.intents.push_back({intent, confidence, confidence});
        else if (intent == "none")
            result.noneMeant = true;
        else if (!intent.empty())
            spdlog::info("[LlmNlu] ignored \"{}\": not an intent of the agent", intent);
    } catch (const std::exception&) {
        spdlog::warn("[LlmNlu] unreadable reply: {}", reply->substr(0, 200));
    }
    return result;
}

} // namespace nlu
} // namespace va
