#pragma once

#include "NluProvider.h"
#include "../llm/Completion.h"
#include "vasdk/flow/agent.pb.h"

#include <memory>
#include <string>
#include <vector>

namespace va {
namespace nlu {

/**
 * @brief Intents by an LLM: it's shown the candidate intents with their examples and picks one,
 *        or none. It can only answer with an intent the agent declares; any other name, or a
 *        reply it can't read, is no intent. Thread-safe.
 *
 * The last stage of a CascadeNluProvider, `{rules, embeddings, llm}`, so the LLM is asked only
 * about what the rules and the on-device model aren't sure of. score and confidence are the
 * LLM's own confidence.
 *
 * The turn waits for it, so it's given up after timeLimitMs (Completion::completeWithin): then
 * it has no answer, and the cascade keeps the earlier stages' best guess.
 */
class VASDK_EXPORT LlmNluProvider : public NluProvider {
public:
    LlmNluProvider(std::shared_ptr<const llm::Completion> completion,
                   const google::protobuf::RepeatedPtrField<vasdk::flow::Intent>& intents, int timeLimitMs = 1500);
    std::string name() const override { return "llm"; }
    NluResult understand(const NluRequest& request) const override;
    int timeLimitMs() const { return _timeLimitMs; }

private:
    std::shared_ptr<const llm::Completion> _completion;
    int _timeLimitMs;
    std::vector<std::pair<std::string, std::vector<std::string>>> _intents;   // name, examples
};

} // namespace nlu
} // namespace va
