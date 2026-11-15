#include "nlu/RuleNluProvider.h"

#include "flow/CompiledAgent.h"
#include "flow/ExamplePattern.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace va {
namespace nlu {

namespace {

// The intents every agent has; their examples come from its languages.
bool builtInName(const std::string& intent) {
    for (const char* name : {"sys.yes", "sys.no", "sys.cancel", "sys.repeat", "sys.help", "sys.skip"})
        if (intent == name) return true;
    return false;
}

} // namespace

RuleNluProvider::RuleNluProvider(const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents,
                                 std::shared_ptr<const Languages> languages, float minSimilarity)
    : _languages(languages ? std::move(languages) : std::make_shared<const Languages>()), _minSimilarity(minSimilarity) {
    for (const auto& intent : intents)
        add(intent.name(), {intent.examples().begin(), intent.examples().end()},
            {intent.negative_examples().begin(), intent.negative_examples().end()});
    // The built-in intents, in the languages' words: patterns too ("(cancel|stop) [it]"), with no sets.
    for (const char* name : {"sys.yes", "sys.no", "sys.cancel", "sys.repeat", "sys.help", "sys.skip"}) {
        if (_intents.count(name)) continue;
        std::vector<std::string> examples;
        for (const auto& phrase : _languages->intentExamples(name)) {
            std::string problem;
            const auto spelled = flow::expandPattern(phrase, &problem);
            if (!spelled) {
                spdlog::warn("[RuleNluProvider] {} phrase \"{}\": {}: taken as written", name, phrase, problem);
                examples.push_back(phrase);
                continue;
            }
            examples.insert(examples.end(), spelled->begin(), spelled->end());
        }
        add(name, examples, {});
    }
}

bool RuleNluProvider::isBuiltIn(const std::string& intent) { return builtInName(intent); }

// "book an @service for @sys.date": the words between entities, and the entities.
void RuleNluProvider::add(const std::string& name, const std::vector<std::string>& examples,
                          const std::vector<std::string>& negatives) {
    if (!_intents.count(name)) _order.push_back(name);
    auto& intent = _intents[name];
    for (const auto& text : examples) {
        Example example;
        std::vector<std::string> segment;
        // The gap before the next segment: any, unless wildcards since the last one say otherwise.
        Gap pending;
        bool wildcards = false;
        auto close = [&] {
            if (!segment.empty()) example.segments.push_back(std::move(segment));
            segment.clear();
        };
        std::istringstream tokens(text);
        std::string token;
        while (tokens >> token) {
            if (token[0] == '@') {
                std::string entity = token.substr(1);
                while (!entity.empty() && !std::isalnum(static_cast<unsigned char>(entity.back())) && entity.back() != '_')
                    entity.pop_back();
                close();
                example.entities.push_back(entity);
                pending = Gap{};   // an entity is anywhere in the words
                wildcards = false;
                continue;
            }
            if (flow::isWildcard(token)) {
                close();
                if (!wildcards) pending = Gap{0, 0};
                wildcards = true;
                if (token == "*") pending.most = Gap{}.most;
                if (token == "+" || token == "_") ++pending.fewest;
                if (token == "_" && pending.most != Gap{}.most) ++pending.most;
                if (token == "+") pending.most = Gap{}.most;
                continue;
            }
            for (auto& word : flow::splitWords(token)) {
                if (segment.empty()) {
                    // Before the first segment: at least so many words, but anywhere after them.
                    Gap gap = wildcards ? pending : Gap{};
                    if (example.segments.empty()) gap.most = Gap{}.most;
                    example.gaps.push_back(gap);
                    pending = Gap{};
                    wildcards = false;
                }
                segment.push_back(std::move(word));
            }
        }
        close();
        if (wildcards) example.trailing = pending.fewest;   // "remind me to _": a word after the last
        for (const auto& words : example.segments) example.size += words.size();
        example.size += example.entities.size();
        if (example.size > 0) intent.examples.push_back(std::move(example));
    }
    for (const auto& text : negatives)
        if (auto words = flow::splitWords(text); !words.empty()) intent.negatives.push_back(std::move(words));
}

std::vector<std::string> RuleNluProvider::entitiesUsed() const {
    std::set<std::string> used;
    for (const auto& [name, intent] : _intents)
        for (const auto& example : intent.examples) used.insert(example.entities.begin(), example.entities.end());
    return {used.begin(), used.end()};
}

float RuleNluProvider::score(const Example& example, const std::vector<std::string>& words, const NluRequest& request,
                             bool negatable) const {
    for (const auto& entity : example.entities)
        if (!request.mentions || !request.mentions(entity)) return 0;
    float total = static_cast<float>(example.entities.size());
    size_t cursor = 0;
    size_t first = words.size();
    for (size_t s = 0; s < example.segments.size(); ++s) {   // in order, each after the one before, its gap apart
        const auto& segment = example.segments[s];
        const Gap& gap = example.gaps[s];
        const auto found = findPhrase(words, segment, _minSimilarity);
        const auto at = std::find_if(found.begin(), found.end(), [cursor, &gap](const auto& match) {
            return match.first >= cursor && match.first - cursor >= gap.fewest && match.first - cursor <= gap.most;
        });
        if (at == found.end()) return 0;
        first = std::min(first, at->first);
        total += at->second;
        cursor = at->first + segment.size();
    }
    if (words.size() - std::min(cursor, words.size()) < example.trailing) return 0;
    if (negatable && first < words.size()) {   // "I don't want to book": a negation just before
        for (size_t at = first > 3 ? first - 3 : 0; at < first; ++at)
            if (_languages->negates(words[at])) return 0;
    }
    return total;
}

NluResult RuleNluProvider::understand(const NluRequest& request) const {
    NluResult result;
    result.stage = name();
    const auto words = flow::splitWords(request.text);
    for (const auto& name : request.candidates.empty() ? _order : request.candidates) {
        const auto intent = _intents.find(name);
        if (intent == _intents.end()) continue;
        const bool excluded = std::any_of(intent->second.negatives.begin(), intent->second.negatives.end(),
                                          [&](const auto& negative) { return !findPhrase(words, negative, 1.0f).empty(); });
        if (excluded) continue;
        const bool negatable = name != "sys.no" && name != "sys.cancel" && name != "sys.skip";
        IntentMatch best;
        for (const auto& example : intent->second.examples) {
            const float matched = score(example, words, request, negatable);
            if (matched > best.score) {
                best.score = matched;
                best.confidence = matched / static_cast<float>(example.size);
                // shortcut: an entity counts as one word however many it spans; measure its span if that misleads.
                best.explains = std::min(1.0f, static_cast<float>(example.size) / static_cast<float>(words.size()));
            }
        }
        if (best.score <= 0) continue;
        best.intent = name;
        result.intents.push_back(std::move(best));
    }
    std::stable_sort(result.intents.begin(), result.intents.end(),
                     [](const IntentMatch& a, const IntentMatch& b) { return a.score > b.score; });
    return result;
}

} // namespace nlu
} // namespace va
