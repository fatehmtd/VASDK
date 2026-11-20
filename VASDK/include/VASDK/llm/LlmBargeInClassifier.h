#pragma once

#include "../BargeInClassifier.h"
#include "Completion.h"

#include <memory>
#include <string>
#include <vector>

namespace va {
namespace llm {

/**
 * @brief Barge-in decisions by an LLM: it's told what the agent is saying (or what the user asked,
 *        while the agent thinks), and what the user said over it, and answers with one word.
 *
 * While the agent speaks: BACKCHANNEL (the agent carries on) or INTERRUPT. While it thinks: ADDS
 * (the words add to or change the request: cancel and answer them) or ACKNOWLEDGES (an
 * acknowledgement or noise: keep the coming reply). The last of those words in the reply counts, so
 * a model that reasons first is read too. Anything else, a failure, or no answer within
 * timeLimitMs (Completion::completeWithin) is Undecided: the word rules decide. Thread-safe.
 *
 * A small, fast model fits: a llm_profiles entry named by turn_taking.backchannel.barge_in_llm_profile.
 */
class VASDK_EXPORT LlmBargeInClassifier : public pipeline::BargeInClassifier {
public:
    explicit LlmBargeInClassifier(std::shared_ptr<const Completion> completion, int timeLimitMs = 800);
    std::string name() const override { return "llm"; }
    pipeline::BackchannelVerdict classify(const pipeline::BargeInContext& context) const override;
    int timeLimitMs() const { return _timeLimitMs; }

    /** The messages classify() sends, for tests and logs. */
    static std::vector<ConversationMessage> messagesFor(const pipeline::BargeInContext& context);
    /** The verdict a model's reply stands for (the last answer word in it), or Undecided. */
    static pipeline::BackchannelVerdict verdictOf(const std::string& reply, bool agentIsThinking);

private:
    std::shared_ptr<const Completion> _completion;
    int _timeLimitMs;
};

} // namespace llm
} // namespace va
