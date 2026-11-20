#include "VASDK/llm/LlmBargeInClassifier.h"

#include <spdlog/spdlog.h>

#include <cctype>

namespace va {
namespace llm {

namespace {
    constexpr const char* kSpeakingInstructions =
        "You watch a voice assistant's conversation. The user said something while the assistant was "
        "still speaking. Decide whether it is a back-channel (the user listens and wants the assistant to "
        "carry on: \"mm-hmm\", \"right\", \"okay\", repeating or confirming what it says) or an "
        "interruption (the user wants the assistant to stop: a question, an objection, a correction, a "
        "new request). Answer with one word: BACKCHANNEL or INTERRUPT.";
    constexpr const char* kThinkingInstructions =
        "You watch a voice assistant's conversation. The user made a request, and while the assistant "
        "was preparing its answer the user said something more. Decide whether it adds to or changes "
        "the request (more details, a correction, another question) or is only an acknowledgement or "
        "noise (\"okay\", \"hmm\", \"thanks\", a cough). Answer with one word: ADDS or ACKNOWLEDGES.";
} // namespace

LlmBargeInClassifier::LlmBargeInClassifier(std::shared_ptr<const Completion> completion, int timeLimitMs)
    : _completion(std::move(completion)), _timeLimitMs(timeLimitMs) {}

namespace {
    std::string situationOf(const pipeline::BargeInContext& context) {
        std::string situation;
        if (!context.conversationSummary.empty()) situation += "Conversation so far: " + context.conversationSummary + "\n";
        if (context.agentIsThinking) {
            situation += "The user asked: \"" + context.lastUserTurn + "\"\n";
            situation += "While the assistant was preparing its answer, the user said: \"" + context.heard + "\"";
        } else {
            if (!context.lastUserTurn.empty()) situation += "The user's previous turn: \"" + context.lastUserTurn + "\"\n";
            situation += "The assistant is saying: \"" + context.agentReply + "\"\n";
            situation += "Over it, the user said: \"" + context.heard + "\"";
        }
        // Last, so a small model labels it rather than answering the user ("OKAY").
        return situation + (context.agentIsThinking ? "\nADDS or ACKNOWLEDGES?" : "\nBACKCHANNEL or INTERRUPT?");
    }

    struct Example {
        const char* heard;
        const char* answer;
    };
    // Worked answers: without them a small model (LFM2.5 1.2B) gives one label whatever was said.
    // Another domain than an app's, so they teach the labels, not the words.
    constexpr const char* kExampleRequest = "How do I get to the station?";
    constexpr const char* kExampleReply = "Turn left on Main Street, then keep going for two miles until the bridge.";
    constexpr Example kSpeakingExamples[] = {
        {"uh-huh", "BACKCHANNEL"},
        {"wait, which street?", "INTERRUPT"},
        {"Main Street, got it", "BACKCHANNEL"},
        {"no, I'm walking", "INTERRUPT"},
    };
    constexpr Example kThinkingExamples[] = {
        {"on foot", "ADDS"},
        {"hmm", "ACKNOWLEDGES"},
        {"actually, the airport", "ADDS"},
        {"alright", "ACKNOWLEDGES"},
    };
} // namespace

std::vector<ConversationMessage> LlmBargeInClassifier::messagesFor(const pipeline::BargeInContext& context) {
    std::vector<ConversationMessage> messages{
        {MessageRole::SYSTEM, context.agentIsThinking ? kThinkingInstructions : kSpeakingInstructions, 0}};
    for (const auto& example : context.agentIsThinking ? kThinkingExamples : kSpeakingExamples) {
        pipeline::BargeInContext shown;
        shown.agentIsThinking = context.agentIsThinking;
        shown.lastUserTurn = kExampleRequest;
        shown.agentReply = kExampleReply;
        shown.heard = example.heard;
        messages.push_back({MessageRole::USER, situationOf(shown), 0});
        messages.push_back({MessageRole::ASSISTANT, example.answer, 0});
    }
    messages.push_back({MessageRole::USER, situationOf(context), 0});   // last: the overlap being judged
    return messages;
}

pipeline::BackchannelVerdict LlmBargeInClassifier::verdictOf(const std::string& reply, bool agentIsThinking) {
    const std::string interrupt = agentIsThinking ? "ADDS" : "INTERRUPT";
    const std::string backchannel = agentIsThinking ? "ACKNOWLEDGES" : "BACKCHANNEL";
    auto verdict = pipeline::BackchannelVerdict::Undecided;
    std::string word;
    // The last answer word counts: a model may reason before it answers.
    for (size_t index = 0; index <= reply.size(); ++index) {
        const unsigned char character = index < reply.size() ? static_cast<unsigned char>(reply[index]) : ' ';
        if (std::isalpha(character)) { word += static_cast<char>(std::toupper(character)); continue; }
        if (word == interrupt) verdict = pipeline::BackchannelVerdict::Interrupt;
        else if (word == backchannel) verdict = pipeline::BackchannelVerdict::Backchannel;
        word.clear();
    }
    return verdict;
}

pipeline::BackchannelVerdict LlmBargeInClassifier::classify(const pipeline::BargeInContext& context) const {
    if (!_completion || context.heard.find_first_not_of(" \t\n") == std::string::npos)
        return pipeline::BackchannelVerdict::Undecided;
    const auto reply = _completion->completeWithin(messagesFor(context), _timeLimitMs);
    if (!reply) {
        spdlog::info("[LlmBargeIn] no answer (failed, or not within {} ms): the word rules decide", _timeLimitMs);
        return pipeline::BackchannelVerdict::Undecided;
    }
    const auto verdict = verdictOf(*reply, context.agentIsThinking);
    if (verdict == pipeline::BackchannelVerdict::Undecided)
        spdlog::info("[LlmBargeIn] unreadable answer: the word rules decide");
    return verdict;
}

} // namespace llm
} // namespace va
