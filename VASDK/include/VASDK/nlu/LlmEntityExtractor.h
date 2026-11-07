#pragma once

#include "EntityExtractor.h"
#include "../llm/Completion.h"

#include <memory>

namespace va {
namespace nlu {

/**
 * @brief Entities found by an LLM: it's shown the agent's list entities (label, description,
 *        some values) and the user's words, and copies out the words that say each one. Words it
 *        returns that aren't in the sentence are ignored. Needs the network; a sensitive answer
 *        never reaches it. The turn waits for it, so it's given up after `timeLimitMs`
 *        (Completion::completeWithin): then nothing is found by it. Thread-safe.
 */
class VASDK_EXPORT LlmEntityExtractor : public EntityExtractor {
public:
    explicit LlmEntityExtractor(std::shared_ptr<const llm::Completion> completion, int timeLimitMs = 1500);
    std::string name() const override { return "llm"; }
    std::vector<EntitySpan> extract(const std::vector<std::string>& words, const std::vector<EntityLabel>& labels,
                                    bool sensitive) const override;

private:
    std::shared_ptr<const llm::Completion> _completion;
    int _timeLimitMs;
};

} // namespace nlu
} // namespace va
