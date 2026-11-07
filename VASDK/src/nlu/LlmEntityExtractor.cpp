#include "nlu/LlmEntityExtractor.h"

#include "flow/Entities.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace va {
namespace nlu {

LlmEntityExtractor::LlmEntityExtractor(std::shared_ptr<const llm::Completion> completion, int timeLimitMs)
    : _completion(std::move(completion)), _timeLimitMs(timeLimitMs) {}

std::vector<EntitySpan> LlmEntityExtractor::extract(const std::vector<std::string>& words, const std::vector<EntityLabel>& labels,
                                                    bool sensitive) const {
    if (!_completion || sensitive || words.empty() || labels.empty()) return {};
    std::string listing, said;
    for (const auto& label : labels) {
        listing += "- " + label.entity + " (" + label.label + ")";
        if (!label.description.empty()) listing += ": " + label.description;
        for (size_t i = 0; i < label.examples.size() && i < 8; ++i) listing += (i ? ", " : "; e.g. ") + label.examples[i];
        listing += "\n";
    }
    for (const auto& word : words) said += (said.empty() ? "" : " ") + word;
    const std::string instructions =
        "You find values in what a user of a voice assistant said. The kinds of value:\n" + listing +
        "Reply with only a JSON object: {\"entities\": [{\"entity\": \"<a kind's name>\", \"words\": \"<the user's words "
        "that say it, copied exactly>\"}]}. A value can be said in other words than the examples. An empty list "
        "when none is said.";
    const auto reply = _completion->completeWithin({{llm::MessageRole::SYSTEM, instructions, 0},
                                                    {llm::MessageRole::USER, said, 0}}, _timeLimitMs);
    if (!reply) {
        spdlog::info("[LlmEntities] no answer (failed, or not within {} ms): nothing found", _timeLimitMs);
        return {};
    }
    std::vector<EntitySpan> spans;
    try {
        const auto answer = nlohmann::json::parse(llm::jsonObjectIn(*reply));
        for (const auto& item : answer.value("entities", nlohmann::json::array())) {
            const auto entity = item.value("entity", std::string());
            if (std::none_of(labels.begin(), labels.end(), [&](const EntityLabel& label) { return label.entity == entity; })) continue;
            const auto phrase = flow::entityWords(item.value("words", std::string()));
            if (phrase.empty() || phrase.size() > words.size()) continue;
            // Where the words it copied are in the sentence; not there, it made them up.
            for (size_t start = 0; start + phrase.size() <= words.size(); ++start) {
                if (std::equal(phrase.begin(), phrase.end(), words.begin() + static_cast<long>(start))) {
                    spans.push_back({entity, start, phrase.size(), 0.9f});
                    break;
                }
            }
        }
    } catch (const std::exception&) {
        spdlog::warn("[LlmEntities] unreadable reply: {}", reply->substr(0, 200));
    }
    return nonOverlapping(std::move(spans));
}

} // namespace nlu
} // namespace va
